# KWin Minimize2Tray

**This is work in progress, don't expect feature parity with other implementations right away**

Hide windows to the system tray, similar to [KDocker](https://github.com/user-none/KDocker) but in the form of a KWin Script that works on KDE Plasma 6.0+ Wayland.

![preview](screenshots/preview.png)

> [!IMPORTANT]
> Some applications like Google Chrome or Chromium/Electron based may run in Xwayland mode and have issues hiding from taskbar or minimizing on startup, enabling native Wayland support for them can help. Usually setting an environment variable or command-line argument with KDE's `kmenuedit` is needed, refer to [Wayland - ArchWiki](https://wiki.archlinux.org/title/Wayland#GUI_libraries) or search "linux APP_NAME enable wayland support".
>
> To verify if an application is running on Xwayland instead of Wayland use the KWin Debug Console.
> ![wayland](screenshots/wayland.png)

## Features

- Wayland support (X11 hasn't been tested but was [confirmed to work](https://github.com/luisbocanegra/kwin-minimize2tray/issues/25))
- Should work with windows from any application **as long as they are minimizable**
- Windows are hidden from Task Manager (taskbar), Task Switcher (Alt+Tab), Desktop Pager and restored when clicking on the tray icon
- Per application visibility can be configured from System Tray widget configuration
- Uses the desktop-file icon, including user overrides, with the window icon as fallback
- Application and window status
  - [Unity LauncherAPI](https://wiki.ubuntu.com/Unity/LauncherAPI)
    - Counter: Top-right corner, with a configurable display limit (default `99+`) and optional dot style
    - Progress bar: Bottom bar using the system highlight color
    - Urgent: Sets *[NeedsAttention](https://api.kde.org/plasma-types.html#ItemStatus-enum)* status of icon

    **Note:** Unity LauncherAPI doesn't specify a window so all the tray icons from the same application will get the same status

    Counters must be supplied by the application. Notification history, tray images and tooltips are not treated as unread counts.

  - *[Demands attention](https://invent.kde.org/plasma/kwin/-/blob/fc98b4bcbd80ba025994c2d31244d984a253ec66/src/window.h#L429)* from KWin will also set *[NeedsAttention](https://api.kde.org/plasma-types.html#ItemStatus-enum)* status of icon and works per window.

## Installation

Proper distribution packages will be available eventually, for now you can install it manually.

**You will need to install any required dependencies**, see [#9 Note build dependencies](https://github.com/luisbocanegra/kwin-minimize2tray/issues/9)

```sh
git clone https://github.com/luisbocanegra/kwin-minimize2tray.git
cd kwin-minimize2tray
./install.sh
```

### Immutable distributions

1. Run `./install-immutable.sh` to install
2. Add the QML and Qt plugin paths so the tray and native configuration modules can be found:

    Create the file `~/.config/plasma-workspace/env/path.sh` (and folders if they don't exist) with the following:

    ```sh
    export QML_IMPORT_PATH="$HOME/.local/lib64/qml:$HOME/.local/lib/qml:$QML_IMPORT_PATH"
    export QT_PLUGIN_PATH="$HOME/.local/lib64/plugins:$HOME/.local/lib/plugins:$QT_PLUGIN_PATH"
    ```

3. Log-out or reboot to apply the change

### Build version

Set `MINIMIZE2TRAY_VERSION` in `CMakeLists.txt` to choose the version, or leave it empty to use the nearest Git version tag with its leading `v` removed, falling back to `package/metadata.json`.
An explicit `-DMINIMIZE2TRAY_VERSION=1.2.3` overrides this setting.
Configure with `-DPACKAGE_SCRIPT=ON` to generate a `.kwinscript` archive in the build directory.

## Configuration/Usage

Enable/disable

1. *Minimize to tray* from *System Settings* > *Window Management* > *KWin Scripts*

By default the shortcut is set to `Meta+Alt+PgDown`, to change it:

1. *System Settings* > *Keyboard* > *Shortcuts*
2. From the list on the left select *KWin*
3. Search for "Minimize window to tray" and change it to whatever you prefer

Open the script's configuration and use **Add rule → Detect window properties** to select an application window. Give the rule a name and description, then choose the detected properties that identify the windows to hide. Each rule can be edited, enabled, disabled or deleted; search covers names, descriptions and matching properties. **View as text** shows all rules as read-only JSON, with copy and save options.

New rules default to normal application windows and leave child windows and dialogs visible. Separate editors can also be normal windows: use a detected role, instance or title to distinguish them. A preview lists matching open windows and reasons for exclusions. Matching is case-sensitive; selected conditions are combined with AND, and enabled rules with OR. Modal dialogs, system surfaces, Minimize2Tray itself and its configuration hosts (System Settings and kcmshell6) cannot be hidden, including through the shortcut. Other dialog/utility windows require explicit rules.

Edits remain pending until **Apply** or **OK** on the main configuration page. Newly matching open windows are hidden immediately; disabling, deleting or narrowing their last matching rule restores them and removes their tray icons. Badge settings update existing icons. Deletions can be undone before applying, and Cancel discards pending edits. To preview open windows, the script must be enabled; window detection also works while it is disabled.

Previous comma-separated class settings migrate automatically on first use. The original list is retained as `legacyClassBackup` in the same `kwinrc` script group; imported rules preserve class-or-instance matching and do not guess missing roles. An empty new rule list remains empty. Invalid or unsupported JSON is reported rather than overwritten; the running script keeps its last valid rules. Concurrent edits are detected before saving.

Dialog sizes, page selection, search and selected rule are remembered in the existing script configuration group. Window position restoration is supported on X11; Wayland placement remains controlled by KWin. The native configuration module is built and installed alongside the tray plugin and requires KF6 KCMUtils development files. After installing updated native plugins, start a new Plasma session once before live checks.

## Acknowledgements

[Unity LauncherAPI](https://wiki.ubuntu.com/Unity/LauncherAPI) implementation is a simplified version of [KDE/plasma-desktop/applets/taskmanager/plugin/smartlaunchers](https://github.com/KDE/plasma-desktop/tree/e3ba92b113d8a4e2d47a589835e9d867059dc2b9/applets/taskmanager/plugin/smartlaunchers)

## Support the development

If this project has been useful to you, consider sponsoring or donating so I can dedicate more time to maintain and improve this and [my other open source work](https://github.com/luisbocanegra?tab=repositories&q=&type=source&language=&sort=stargazers)

[![GitHub Sponsors](https://img.shields.io/badge/GitHub%20Sponsors-%2329313C?logo=githubsponsors&style=for-the-badge)](https://github.com/sponsors/luisbocanegra)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-supporter?logo=ko-fi&logoColor=%23ffffff&color=%23467BEB&style=for-the-badge)](https://ko-fi.com/luisbocanegra)
[!["Buy Me A Coffee"](https://img.shields.io/badge/Buy%20me%20a%20coffee-supporter?logo=buymeacoffee&logoColor=%23282828&color=%23FF803F&style=for-the-badge)](https://www.buymeacoffee.com/luisbocanegra)
[![PayPal](https://img.shields.io/badge/PayPal-supporter?logo=paypal&logoColor=%23ffffff&color=%23003087&style=for-the-badge)](https://www.paypal.com/donate/?hosted_button_id=Y5TMH3Z4YZRDA)
[![Patreon](https://img.shields.io/badge/Patreon-%23000000?logo=patreon&logoColor=%23ffffff&style=for-the-badge)](https://patreon.com/luisbocanegra?utm_medium=unknown&utm_source=join_link&utm_campaign=creatorshare_creator&utm_content=copyLink)
