#include "sessionprocess.h"
#include "sessionpeer.h"

#include <wayland-server-core.h>
#include <wayland/clientconnection.h>
#include <wayland/surface.h>
#include <window.h>

#include <sys/socket.h>
#include <unistd.h>

class NativeSessionProcess : public QObject, public SessionProcess {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SessionProcess_iid FILE "sessionprocess-metadata.json")
    Q_INTERFACES(SessionProcess)
  public:
    int openProcess(QObject *object) const override {
#if defined(SO_PEERPIDFD)
        auto window = qobject_cast<KWin::Window *>(object);
        // An XWayland surface belongs to XWayland, not to the application.
        // Never fall back to a client-advertised PID or executable name.
        if (!window || !window->inherits("KWin::WaylandWindow") || window->isDeleted() || !window->surface())
            return -1;
        auto client = window->surface()->client();
        if (!client || client->tearingDown())
            return -1;
        return Minimize2Tray::openPeerProcess(wl_client_get_fd(client->client()));
#endif
        return -1;
    }
};

#include "sessionprocess.moc"
