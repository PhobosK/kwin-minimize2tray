#include "sessionpeer.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

int Minimize2Tray::openPeerProcess(int socket) {
#if defined(SO_PEERPIDFD)
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0)
        return -1;
    if (size != sizeof(credentials)) {
        errno = EPROTO;
        return -1;
    }
    if (credentials.uid != geteuid() || credentials.pid <= 1 || credentials.pid == getpid() ||
        credentials.pid == getppid()) {
        errno = EPERM;
        return -1;
    }
    int descriptor = -1;
    size = sizeof(descriptor);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERPIDFD, &descriptor, &size) == 0 && size == sizeof(descriptor) &&
        descriptor >= 0 && fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0)
        return descriptor;
    const int failure = errno;
    if (descriptor >= 0)
        close(descriptor);
    errno = failure;
#endif
    return -1;
}
