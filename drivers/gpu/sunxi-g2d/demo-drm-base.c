/*
 * G2D + DRM Base Demo - Helper functions for visual demos
 * 
 * Provides:
 * - DRM initialization with double-height buffer for page flipping
 * - G2D integration for hardware-accelerated drawing
 * - Page flip helpers to avoid tearing
 * 
 * Copyright (C) 2025 Sergio Perez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/dma-heap.h>
#include <linux/sunxi_g2d.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

struct drm_display {
	int drm_fd;
	int g2d_fd;
	
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t fb_id;
	
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t size;
	
	/* Double-height buffer for page flipping via panning */
	void *map;
	uint32_t handle;
	
	/* Current page (0 or 1) */
	int current_page;
};

/*
 * Find first connected connector
 */
static uint32_t find_connector(int drm_fd, drmModeRes *res)
{
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
		if (!conn)
			continue;
			
		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
			uint32_t id = conn->connector_id;
			drmModeFreeConnector(conn);
			return id;
		}
		
		drmModeFreeConnector(conn);
	}
	
	return 0;
}

/*
 * Find CRTC for connector
 */
static uint32_t find_crtc(int drm_fd, drmModeRes *res, drmModeConnector *conn)
{
	drmModeEncoder *enc;
	
	/* Try current encoder first */
	if (conn->encoder_id) {
		enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
		if (enc) {
			if (enc->crtc_id) {
				uint32_t crtc_id = enc->crtc_id;
				drmModeFreeEncoder(enc);
				return crtc_id;
			}
			drmModeFreeEncoder(enc);
		}
	}
	
	/* Try first possible CRTC */
	for (int i = 0; i < conn->count_encoders; i++) {
		enc = drmModeGetEncoder(drm_fd, conn->encoders[i]);
		if (!enc)
			continue;
			
		for (int j = 0; j < res->count_crtcs; j++) {
			if (enc->possible_crtcs & (1 << j)) {
				uint32_t crtc_id = res->crtcs[j];
				drmModeFreeEncoder(enc);
				return crtc_id;
			}
		}
		
		drmModeFreeEncoder(enc);
	}
	
	return 0;
}

/*
 * Initialize DRM display with double-height buffer
 */
int drm_display_init(struct drm_display *disp)
{
	drmModeRes *res;
	drmModeConnector *conn;
	struct drm_mode_create_dumb create = {0};
	struct drm_mode_map_dumb map = {0};
	int ret;
	
	memset(disp, 0, sizeof(*disp));
	disp->current_page = 0;
	
	/* Open DRM device */
	disp->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (disp->drm_fd < 0) {
		perror("open /dev/dri/card0");
		return -1;
	}
	
	/* Open G2D device */
	disp->g2d_fd = open("/dev/g2d", O_RDWR | O_CLOEXEC);
	if (disp->g2d_fd < 0) {
		perror("open /dev/g2d");
		close(disp->drm_fd);
		return -1;
	}
	
	/* Get resources */
	res = drmModeGetResources(disp->drm_fd);
	if (!res) {
		perror("drmModeGetResources");
		goto err_close;
	}
	
	/* Find connector */
	disp->conn_id = find_connector(disp->drm_fd, res);
	if (!disp->conn_id) {
		fprintf(stderr, "No connected display found\n");
		drmModeFreeResources(res);
		goto err_close;
	}
	
	conn = drmModeGetConnector(disp->drm_fd, disp->conn_id);
	if (!conn) {
		perror("drmModeGetConnector");
		drmModeFreeResources(res);
		goto err_close;
	}
	
	/* Copy first mode */
	memcpy(&disp->mode, &conn->modes[0], sizeof(disp->mode));
	disp->width = disp->mode.hdisplay;
	disp->height = disp->mode.vdisplay;
	
	printf("Display: %dx%d @ %dHz\n", 
	       disp->width, disp->height, disp->mode.vrefresh);
	
	/* Find CRTC */
	disp->crtc_id = find_crtc(disp->drm_fd, res, conn);
	if (!disp->crtc_id) {
		fprintf(stderr, "No CRTC found\n");
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		goto err_close;
	}
	
	/* Save current CRTC to restore later */
	disp->saved_crtc = drmModeGetCrtc(disp->drm_fd, disp->crtc_id);
	
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	
	/* Create dumb buffer with DOUBLE HEIGHT for page flipping */
	create.width = disp->width;
	create.height = disp->height * 2;  /* Double height! */
	create.bpp = 32;  /* XRGB8888 */
	
	ret = drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		goto err_restore;
	}
	
	disp->handle = create.handle;
	disp->pitch = create.pitch;
	disp->size = create.size;
	
	printf("Buffer: %dx%d (double height), pitch=%u, size=%u\n",
	       disp->width, disp->height * 2, disp->pitch, disp->size);
	
	/* Create framebuffer (full double-height) */
	ret = drmModeAddFB(disp->drm_fd, disp->width, disp->height * 2, 
	                   24, 32, disp->pitch, disp->handle, &disp->fb_id);
	if (ret) {
		perror("drmModeAddFB");
		goto err_destroy_dumb;
	}
	
	/* Map buffer */
	map.handle = disp->handle;
	ret = drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		goto err_rm_fb;
	}
	
	disp->map = mmap(0, disp->size, PROT_READ | PROT_WRITE, 
	                 MAP_SHARED, disp->drm_fd, map.offset);
	if (disp->map == MAP_FAILED) {
		perror("mmap");
		goto err_rm_fb;
	}
	
	/* Clear both pages */
	memset(disp->map, 0, disp->size);
	
	/* Set mode (displays page 0 initially) */
	ret = drmModeSetCrtc(disp->drm_fd, disp->crtc_id, disp->fb_id, 
	                     0, 0, &disp->conn_id, 1, &disp->mode);
	if (ret) {
		perror("drmModeSetCrtc");
		goto err_unmap;
	}
	
	printf("DRM display initialized successfully\n");
	printf("Page 0: offset 0, Page 1: offset %u bytes\n", 
	       disp->pitch * disp->height);
	
	return 0;
	
err_unmap:
	munmap(disp->map, disp->size);
err_rm_fb:
	drmModeRmFB(disp->drm_fd, disp->fb_id);
err_destroy_dumb:
	{
		struct drm_mode_destroy_dumb destroy = { .handle = disp->handle };
		drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
err_restore:
	if (disp->saved_crtc) {
		drmModeSetCrtc(disp->drm_fd, disp->saved_crtc->crtc_id,
		               disp->saved_crtc->buffer_id,
		               disp->saved_crtc->x, disp->saved_crtc->y,
		               &disp->conn_id, 1, &disp->saved_crtc->mode);
		drmModeFreeCrtc(disp->saved_crtc);
	}
err_close:
	close(disp->g2d_fd);
	close(disp->drm_fd);
	return -1;
}

/*
 * Cleanup DRM display
 */
void drm_display_cleanup(struct drm_display *disp)
{
	struct drm_mode_destroy_dumb destroy = { .handle = disp->handle };
	
	/* Restore original CRTC */
	if (disp->saved_crtc) {
		drmModeSetCrtc(disp->drm_fd, disp->saved_crtc->crtc_id,
		               disp->saved_crtc->buffer_id,
		               disp->saved_crtc->x, disp->saved_crtc->y,
		               &disp->conn_id, 1, &disp->saved_crtc->mode);
		drmModeFreeCrtc(disp->saved_crtc);
	}
	
	munmap(disp->map, disp->size);
	drmModeRmFB(disp->drm_fd, disp->fb_id);
	drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	
	close(disp->g2d_fd);
	close(disp->drm_fd);
	
	printf("DRM display cleaned up\n");
}

/*
 * Get pointer to backbuffer (page to draw into)
 */
void *drm_get_backbuffer(struct drm_display *disp)
{
	int next_page = 1 - disp->current_page;
	return (uint8_t *)disp->map + (next_page * disp->pitch * disp->height);
}

/*
 * Get offset of backbuffer in bytes
 */
uint32_t drm_get_backbuffer_offset(struct drm_display *disp)
{
	int next_page = 1 - disp->current_page;
	return next_page * disp->pitch * disp->height;
}

/*
 * Flip to backbuffer using panning (tear-free)
 */
int drm_flip_page(struct drm_display *disp)
{
	int next_page = 1 - disp->current_page;
	int y_offset = next_page * disp->height;
	int ret;
	
	/* Pan display to next page */
	ret = drmModeSetCrtc(disp->drm_fd, disp->crtc_id, disp->fb_id,
	                     0, y_offset,  /* x_offset, y_offset */
	                     &disp->conn_id, 1, &disp->mode);
	if (ret) {
		perror("drmModeSetCrtc (pan)");
		return -1;
	}
	
	disp->current_page = next_page;
	return 0;
}

/*
 * Export DRM dumb buffer as DMA-BUF for G2D
 * Note: Returns fd that must be closed by caller
 */
int drm_export_dmabuf(struct drm_display *disp)
{
	struct drm_prime_handle prime = {
		.handle = disp->handle,
		.flags = DRM_CLOEXEC | DRM_RDWR,
		.fd = -1,
	};
	int ret;
	
	ret = drmIoctl(disp->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (ret) {
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
		return -1;
	}
	
	return prime.fd;
}

/*
 * Helper: Fill rectangle using G2D
 */
int g2d_fillrect_display(struct drm_display *disp, 
                         int x, int y, int w, int h, uint32_t color)
{
	struct g2d_fillrect fill = {0};
	int dmabuf_fd;
	int ret;
	int backbuffer_y_offset;
	
	/* Export DRM buffer as DMA-BUF */
	dmabuf_fd = drm_export_dmabuf(disp);
	if (dmabuf_fd < 0)
		return -1;
	
	/* Calculate Y offset for current backbuffer page (in lines) */
	backbuffer_y_offset = drm_get_backbuffer_offset(disp) / disp->pitch;
	
	/* Configure fillrect operation on full buffer */
	fill.dst.width = disp->width;
	fill.dst.height = disp->height * 2;  /* Full buffer height */
	fill.dst.format = G2D_FMT_XRGB8888;
	fill.dst.stride[0] = disp->pitch;
	fill.dst.dma_fd = dmabuf_fd;
	
	/* Rectangle position (absolute within full buffer) and size */
	fill.dst_x = x;
	fill.dst_y = y + backbuffer_y_offset;  /* Offset to current page */
	fill.dst_w = w;
	fill.dst_h = h;
	fill.color = color;
	fill.fence_fd_in = -1;
	fill.fence_fd_out = -1;
	
	ret = ioctl(disp->g2d_fd, G2D_IOC_FILLRECT, &fill);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT");
		close(dmabuf_fd);
		return -1;
	}
	
	close(dmabuf_fd);
	return 0;
}

/*
 * Helper: Clear backbuffer
 */
int g2d_clear_backbuffer(struct drm_display *disp, uint32_t color)
{
	return g2d_fillrect_display(disp, 0, 0, disp->width, disp->height, color);
}
