/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DRM + G2D Demo - Shared definitions
 * Copyright (C) 2025 Sergio Perez
 */

#ifndef DEMO_DRM_BASE_H
#define DEMO_DRM_BASE_H

#include <stdint.h>
#include <xf86drmMode.h>

/* GLOBAL: Track current page separately to avoid struct corruption bug */
extern volatile int g_current_page;

/*
 * DRM display context
 */
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
	
	/* Per-page framebuffer IDs for separate dmabuf export */
	uint32_t fb_page0_id;
	uint32_t fb_page1_id;
	
	/* current_page REMOVED - now using global g_current_page */
};

/* Function declarations */
int drm_display_init(struct drm_display *disp);
void drm_display_cleanup(struct drm_display *disp);
void *drm_get_backbuffer(struct drm_display *disp);
uint32_t drm_get_backbuffer_offset(struct drm_display *disp);
int drm_flip_page(struct drm_display *disp);
int drm_flip_page_vsync(struct drm_display *disp);  /* Flip with VSYNC wait */
int drm_export_dmabuf(struct drm_display *disp);
int drm_export_page_dmabuf(struct drm_display *disp, int page);  /* Export specific page (0 or 1) */
int g2d_fillrect_display(struct drm_display *disp, int x, int y, int w, int h, uint32_t color);
int g2d_clear_backbuffer(struct drm_display *disp, uint32_t color);

#endif /* DEMO_DRM_BASE_H */
