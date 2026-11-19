/* Minimal replacement for Android's libsync (not available for the HarmonyOS/musl sysroot): sync_wait() only. */
#ifndef LIBSYNC_SHIM_H
#define LIBSYNC_SHIM_H
#include <errno.h>
#include <poll.h>

static inline int sync_wait(int fd, int timeout)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret;
    do {
        ret = poll(&pfd, 1, timeout);
    } while (ret < 0 && errno == EINTR);
    if (ret == 0) {
        errno = ETIME;
        return -1;
    }
    return ret < 0 ? -1 : 0;
}
#endif
