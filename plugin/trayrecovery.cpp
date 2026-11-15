// A bounded, unprivileged worker: only repair stale transparent XEmbedSNIProxy input containers.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <poll.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <xcb/res.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>

namespace {
template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
template <typename T> Reply<T> owned(T *value) { return Reply<T>(value, &std::free); }
constexpr char ProxyClass[] = "xembedsniproxy\0xembedsniproxy";
bool proxyProcess(uint32_t pid) {
    if (pid <= 1)
        return false;
    const auto proc = std::string("/proc/") + std::to_string(pid);
    struct stat attributes{};
    if (stat(proc.c_str(), &attributes) || attributes.st_uid != getuid())
        return false;
    char executable[4096];
    const auto length = readlink((proc + "/exe").c_str(), executable, sizeof(executable) - 1);
    if (length <= 0 || size_t(length) == sizeof(executable) - 1)
        return false;
    executable[length] = 0;
    const auto name = strrchr(executable, '/');
    return name && !strcmp(name + 1, "xembedsniproxy");
}
// KWin may not inherit the XWayland DISPLAY used by the proxy. Import only
// the two X connection variables, never the proxy's executable/search-path environment.
bool proxyDisplay(uint32_t pid) {
    std::ifstream file(std::string("/proc/") + std::to_string(pid) + "/environ", std::ios::binary);
    if (!file)
        return false;
    std::string environment(1024 * 1024, '\0');
    file.read(environment.data(), environment.size());
    const auto length = file.gcount();
    if (length <= 0 || !file.eof())
        return false;
    environment.resize(length);
    std::string display, authority;
    for (size_t pos = 0; pos < environment.size();) {
        const auto end = environment.find('\0', pos);
        if (end == std::string::npos)
            return false;
        const auto value = environment.substr(pos, end - pos);
        if (value.starts_with("DISPLAY="))
            display = value.substr(8);
        if (value.starts_with("XAUTHORITY="))
            authority = value.substr(11);
        pos = end + 1;
    }
    if (display.size() < 2 || display.size() > 32 || display.front() != ':' || display[1] < '0' || display[1] > '9' ||
        display.find_first_not_of("0123456789.", 1) != std::string::npos ||
        std::count(display.begin(), display.end(), '.') > 1 || display.back() == '.')
        return false;
    if (!authority.empty() && authority.front() != '/')
        return false;
    setenv("DISPLAY", display.c_str(), true);
    if (authority.empty())
        unsetenv("XAUTHORITY");
    else
        setenv("XAUTHORITY", authority.c_str(), true);
    return true;
}
uint32_t ownerPid(xcb_connection_t *connection, xcb_window_t window) {
    const xcb_res_client_id_spec_t spec{window, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
    auto reply =
        owned(xcb_res_query_client_ids_reply(connection, xcb_res_query_client_ids(connection, 1, &spec), nullptr));
    if (!reply)
        return 0;
    auto it = xcb_res_query_client_ids_ids_iterator(reply.get());
    for (; it.rem; xcb_res_client_id_value_next(&it))
        if (it.data->spec.mask == XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID &&
            xcb_res_client_id_value_value_length(it.data) == 1)
            return *xcb_res_client_id_value_value(it.data);
    return 0;
}
xcb_atom_t atom(xcb_connection_t *connection, const char *name) {
    auto reply =
        owned(xcb_intern_atom_reply(connection, xcb_intern_atom(connection, true, strlen(name), name), nullptr));
    return reply ? reply->atom : xcb_atom_t(XCB_NONE);
}
bool buttonsHeld(xcb_connection_t *connection, xcb_window_t root) {
    auto reply = owned(xcb_query_pointer_reply(connection, xcb_query_pointer(connection, root), nullptr));
    return !reply || reply->mask & (XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3 | XCB_BUTTON_MASK_4 |
                                    XCB_BUTTON_MASK_5);
}
std::vector<xcb_rectangle_t> candidate(xcb_connection_t *connection, xcb_window_t window, xcb_atom_t opacity,
                                       const std::set<uint32_t> &pids) {
    const auto attributesCookie = xcb_get_window_attributes(connection, window);
    const auto classCookie = xcb_get_property(connection, false, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 64);
    auto attributes = owned(xcb_get_window_attributes_reply(connection, attributesCookie, nullptr));
    auto klass = owned(xcb_get_property_reply(connection, classCookie, nullptr));
    if (!attributes || !attributes->override_redirect || attributes->map_state != XCB_MAP_STATE_VIEWABLE || !klass ||
        klass->format != 8 || xcb_get_property_value_length(klass.get()) != sizeof(ProxyClass) ||
        memcmp(xcb_get_property_value(klass.get()), ProxyClass, sizeof(ProxyClass)))
        return {};
    const auto pid = ownerPid(connection, window);
    if (!pids.contains(pid) || !proxyProcess(pid))
        return {};
    auto geometry = owned(xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr));
    if (!geometry || !geometry->width || !geometry->height || geometry->width > 128 || geometry->height > 128)
        return {};
    auto transparent = owned(xcb_get_property_reply(
        connection, xcb_get_property(connection, false, window, opacity, XCB_ATOM_CARDINAL, 0, 1), nullptr));
    if (!transparent || transparent->format != 32 || xcb_get_property_value_length(transparent.get()) != 4 ||
        *static_cast<const uint32_t *>(xcb_get_property_value(transparent.get())) != 0)
        return {};
    auto shape = owned(xcb_shape_get_rectangles_reply(
        connection, xcb_shape_get_rectangles(connection, window, XCB_SHAPE_SK_INPUT), nullptr));
    if (!shape)
        return {};
    const auto length = xcb_shape_get_rectangles_rectangles_length(shape.get());
    if (length <= 0 || length > 16)
        return {};
    const auto rectangles = xcb_shape_get_rectangles_rectangles(shape.get());
    return {rectangles, rectangles + length};
}
} // namespace

int main(int argc, char **argv) {
    const auto session = getenv("XDG_SESSION_TYPE");
    if (argc < 2 || argc > 33 || !session || strcmp(session, "wayland"))
        return 1;
    std::set<uint32_t> pids;
    for (int i = 1; i < argc; ++i) {
        char *end = nullptr;
        const auto number = strtoul(argv[i], &end, 10);
        if (!*argv[i] || *end || number > UINT32_MAX || !proxyProcess(number))
            return 1;
        pids.insert(number);
    }
    if (!proxyDisplay(*pids.begin()))
        return 2;
    int screenNumber = 0;
    auto connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>(xcb_connect(nullptr, &screenNumber),
                                                                                   &xcb_disconnect);
    auto c = connection.get();
    if (!c || xcb_connection_has_error(c))
        return 2;
    const auto shapeExtension = xcb_get_extension_data(c, &xcb_shape_id);
    const auto resExtension = xcb_get_extension_data(c, &xcb_res_id);
    if (!shapeExtension || !shapeExtension->present || !resExtension || !resExtension->present)
        return 2;
    auto screen = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; screen.rem && i < screenNumber; ++i)
        xcb_screen_next(&screen);
    if (!screen.rem)
        return 2;
    const auto root = screen.data->root;
    const auto opacity = atom(c, "_NET_WM_WINDOW_OPACITY");
    if (!opacity || buttonsHeld(c, root))
        return 0;
    auto tree = owned(xcb_query_tree_reply(c, xcb_query_tree(c, root), nullptr));
    if (!tree || xcb_query_tree_children_length(tree.get()) > 4096)
        return 2;
    struct Container {
        xcb_window_t window;
        std::vector<xcb_rectangle_t> input;
    };
    std::vector<Container> containers;
    const auto children = xcb_query_tree_children(tree.get());
    for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
        auto input = candidate(c, children[i], opacity, pids);
        if (input.empty())
            continue;
        containers.push_back({children[i], std::move(input)});
        xcb_shape_select_input(c, children[i], true);
        const uint32_t events = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(c, children[i], XCB_CW_EVENT_MASK, &events);
    }
    if (containers.empty())
        return 0;
    xcb_flush(c);
    // Normal click forwarding clears input within 300 ms. Observe a full second,
    // and leave containers alone if their shape/geometry changes in that time.
    std::set<xcb_window_t> changed;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    pollfd descriptor{xcb_get_file_descriptor(c), POLLIN, 0};
    while (std::chrono::steady_clock::now() < until && !xcb_connection_has_error(c)) {
        if (buttonsHeld(c, root))
            return 0;
        while (auto raw = xcb_poll_for_event(c)) {
            auto event = owned(raw);
            const auto type = event->response_type & 0x7f;
            if (type == shapeExtension->first_event + XCB_SHAPE_NOTIFY)
                changed.insert(reinterpret_cast<xcb_shape_notify_event_t *>(event.get())->affected_window);
            else if (type == XCB_CONFIGURE_NOTIFY)
                changed.insert(reinterpret_cast<xcb_configure_notify_event_t *>(event.get())->window);
            else if (type == XCB_DESTROY_NOTIFY)
                changed.insert(reinterpret_cast<xcb_destroy_notify_event_t *>(event.get())->window);
        }
        poll(&descriptor, 1, 50);
    }
    if (buttonsHeld(c, root))
        return 0;
    unsigned int repaired = 0;
    for (const auto &container : containers) {
        if (changed.contains(container.window))
            continue;
        const auto current = candidate(c, container.window, opacity, pids);
        if (current.size() != container.input.size() || current.empty() ||
            memcmp(current.data(), container.input.data(), current.size() * sizeof(xcb_rectangle_t)))
            continue;
        if (buttonsHeld(c, root))
            return 0; // A new click takes precedence over recovery.
        auto error = owned(xcb_request_check(c, xcb_shape_rectangles_checked(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                                                                             XCB_CLIP_ORDERING_UNSORTED,
                                                                             container.window, 0, 0, 0, nullptr)));
        if (!error)
            ++repaired;
    }
    xcb_flush(c);
    if (repaired)
        printf("Minimize2Tray: recovered %u stale XEmbed input regions\n", repaired);
    return 0;
}
