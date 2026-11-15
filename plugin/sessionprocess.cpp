#include "sessionprocess.h"
#include "sessionpeer.h"

#include <wayland-server-core.h>
#include <wayland/clientconnection.h>
#include <wayland/surface.h>
#include <wayland/xdgshell.h>
#include <window.h>

#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

class NativeSessionProcess : public QObject, public SessionProcess {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SessionProcess_iid FILE "sessionprocess-metadata.json")
    Q_INTERFACES(SessionProcess)
  public:
    bool requestClose(QObject *object) const override {
        errno = ENOTSUP;
        const auto window = qobject_cast<KWin::Window *>(object);
        if (!window || !window->inherits("KWin::WaylandWindow") || window->isDeleted() || !window->surface())
            return false;
        const auto client = window->surface()->client();
        if (!client || client->tearingDown() || !client->client()) {
            errno = EPIPE;
            return false;
        }
        struct Search {
            KWin::SurfaceInterface *surface;
            KWin::XdgToplevelInterface *toplevel = nullptr;
            int visited = 0;
        } search{window->surface()};
        // Only exported SDK interfaces are used. Never guess a private window layout.
        wl_client_for_each_resource(
            client->client(),
            [](wl_resource *resource, void *data) {
                auto &search = *static_cast<Search *>(data);
                if (++search.visited > 16384)
                    return WL_ITERATOR_STOP;
                const auto toplevel = KWin::XdgToplevelInterface::get(resource);
                if (toplevel && toplevel->surface() == search.surface) {
                    search.toplevel = toplevel;
                    return WL_ITERATOR_STOP;
                }
                return WL_ITERATOR_CONTINUE;
            },
            &search);
        if (!search.toplevel) {
            errno = search.visited > 16384 ? E2BIG : ENOENT;
            return false;
        }
        // Explicit Quit addresses one surface; never change rules or signal a process.
        search.toplevel->sendClose();
        return true;
    }

    int openProcess(QObject *object) const override {
        errno = ENOTSUP;
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
