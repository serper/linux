#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

/* Prefer system header if installed; fallback to kernel tree UAPI path when
 * building inside the kernel source tree (compile from tools/ directory).
 */
#include <linux/sunxi_g2d.h>

int main(int argc, char **argv)
{
    const char *dev = "/dev/g2d";
    int fd, ret;
    struct g2d_selftest st;
    struct pollfd pfd;
    int timeout_ms = 200; /* default delay before fence is signalled */

    if (argc > 1)
        timeout_ms = atoi(argv[1]);

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open /dev/g2d");
        return 1;
    }

    memset(&st, 0, sizeof(st));
    st.timeout_ms = timeout_ms;
    st.fence_fd = -1;

    printf("issuing selftest ioctl (timeout=%d ms)\n", timeout_ms);
    ret = ioctl(fd, G2D_IOC_SELFTEST_FENCE, &st);
    if (ret < 0) {
        perror("ioctl G2D_IOC_SELFTEST_FENCE");
        close(fd);
        return 1;
    }

    printf("got fence fd: %d\n", st.fence_fd);

    pfd.fd = st.fence_fd;
    pfd.events = POLLIN;

    /* wait up to 5 seconds for fence to signal */
    ret = poll(&pfd, 1, 5000);
    if (ret < 0) {
        perror("poll");
        close(st.fence_fd);
        close(fd);
        return 1;
    } else if (ret == 0) {
        fprintf(stderr, "timeout waiting for fence\n");
        close(st.fence_fd);
        close(fd);
        return 2;
    }

    if (pfd.revents & POLLIN) {
        printf("fence signalled (POLLIN)\n");
    } else {
        printf("fence revents=0x%x\n", pfd.revents);
    }

    close(st.fence_fd);
    close(fd);
    return 0;
}
