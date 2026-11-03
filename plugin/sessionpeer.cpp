#include "sessionpeer.h"

#include <sys/socket.h>
#include <unistd.h>

int Minimize2Tray::openPeerProcess(int socket) {
#if defined(SO_PEERPIDFD)
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || size != sizeof(credentials) ||
        credentials.uid != geteuid() || credentials.pid <= 1 || credentials.pid == getpid() ||
        credentials.pid == getppid())
        return -1;
    int descriptor = -1;
    size = sizeof(descriptor);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERPIDFD, &descriptor, &size) == 0 && size == sizeof(descriptor))
        return descriptor;
    if (descriptor >= 0)
        close(descriptor);
#endif
    return -1;
}
