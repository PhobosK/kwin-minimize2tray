# KWin Minimize2Tray — PhobosK fork

**This is the [PhobosK fork](https://github.com/PhobosK/kwin-minimize2tray) of [KWin Minimize2Tray](https://github.com/luisbocanegra/kwin-minimize2tray), originally created by [Luis Bocanegra](https://github.com/luisbocanegra).**

This fork builds on the original work with reliability fixes, easier configuration and selected KDocker-like features, while preserving Wayland support and existing X11 compatibility.

**This is work in progress, don't expect feature parity with other implementations right away**

Hide windows to the system tray, similar to [KDocker](https://github.com/user-none/KDocker) but in the form of a KWin Script that works on KDE Plasma 6.0+ Wayland.

![preview](screenshots/preview.png)

Some Chromium/Electron applications need native Wayland mode for reliable hiding. Check their window type in KWin Debug Console and see the [Wayland notes](https://wiki.archlinux.org/title/Wayland#GUI_libraries).

![wayland](screenshots/wayland.png)

## Features

- Minimize windows to tray and restore them without changing monitor, geometry, desktop or activity memberships. Showing a window switches to its assigned desktop/activity.
- Searchable per-window rules with KWin detection, matching previews and JSON export. Apply/OK updates running windows immediately.
- Independent startup and minimize actions, plus close-to-tray through an optional shortcut. Child windows, modal dialogs and system/configuration windows are protected by default.
- Desktop-file icons, application-provided [Unity LauncherAPI](https://wiki.ubuntu.com/Unity/LauncherAPI) counters/progress/urgency, and per-window attention. Counts are shared between an app's windows; notifications are not unread counters.
- About and configuration access from tray menus. Plasma currently does not display the action tooltips.
- Wayland support and existing X11 compatibility; X11 support was [reported upstream](https://github.com/luisbocanegra/kwin-minimize2tray/issues/25).

## Installation

Install Python 3.12+, CMake 3.19+, a C++20 compiler, ECM, Qt 6.6+ development files (DBus, Gui, Qml, Widgets) and KF6 development files (Config, CoreAddons, Service, StatusNotifierItem, KCMUtils, XmlGui). The RPM spec lists openSUSE dependencies.

```sh
git clone https://github.com/PhobosK/kwin-minimize2tray.git
cd kwin-minimize2tray
./install.sh
```

Run as a regular user. The installer builds the fork's latest stable `X.Y.Z`/`vX.Y.Z` tag and installs under `/usr`, using sudo only for final copies. It leaves the checkout and existing `build/` alone. Network failures stop installation.

Use `./install.sh --local` for unpublished checkout changes, or `./install.sh --version 0.6.0` for a specific release. Both installers offer `--help` for source selection, commit pinning, prefixes, staged installation and offline archives.

### Immutable/user installation

Run `./install-immutable.sh` to install under `~/.local` without sudo; add `--local` for checkout changes. Put these lines in `~/.config/plasma-workspace/env/path.sh`, creating its parent directories as needed, then log out and back in:

```sh
export QML_IMPORT_PATH="$HOME/.local/lib/qml:$QML_IMPORT_PATH"
export QT_PLUGIN_PATH="$HOME/.local/lib/plugins:$QT_PLUGIN_PATH"
```

For a custom prefix, use the paths printed by the installer. A user installation can shadow a system copy; remove unwanted copies yourself. After updating native plugins, start a new Plasma session.

### openSUSE Tumbleweed RPM

The standalone spec builds this fork only. It defaults to tag `v0.6.0`; `release_version` sets both the RPM and application version. It downloads the selected archive if missing from RPM's `SOURCES` directory.

```sh
rpmbuild -ba distro/rpm/kwin6-script-minimize2tray.spec
```

Select another tagged release:

```sh
rpmbuild -ba --define 'release_version 0.4.0' distro/rpm/kwin6-script-minimize2tray.spec
```

Or select a full commit SHA with an explicit application version:

```sh
rpmbuild -ba --define 'release_version 0.6.0' --define 'commit FULL_SHA' distro/rpm/kwin6-script-minimize2tray.spec
```

The tag format is `vX.Y.Z`. Selected sources must contain this fork's native configuration module and current CMake version option.

### Debian and Ubuntu packages

The version in `distro/debian/changelog` selects the fork's `vX.Y.Z` tag and sets the package/application versions. Preparation needs Python 3.12+ and `dpkg-dev`:

```sh
./distro/debian/prepare.py /tmp/minimize2tray-deb
```

For a specific commit, add `--commit FULL_SHA`. `--archive /path/to/source.tar.gz` uses the matching GitHub archive offline. The output directory must be new. Use a new upstream changelog version when changing orig-archive contents.

In the prepared source directory, install build dependencies and build:

```sh
sudo apt-get build-dep ./
dpkg-buildpackage -us -uc
```

This targets Debian 13 and Ubuntu 26.10 with Plasma 6 development packages (Ubuntu needs Universe). Source selection and its checksum are recorded in the prepared package; the build performs no tag lookup or download. No container is required.

### Developer build

```sh
cmake -S . -B build
cmake --build build
```

Set `MINIMIZE2TRAY_VERSION` in CMake or pass `-DMINIMIZE2TRAY_VERSION=1.2.3`. Otherwise CMake uses the nearest matching Git tag, then source metadata. `-DPACKAGE_SCRIPT=ON` creates a `.kwinscript` archive. Compilation commands are in `build/compile_commands.json`; disable them with `-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF`.

## Configuration and use

Enable **Minimize to Tray** in **System Settings → Window Management → KWin Scripts**. The default shortcut is **Meta+Alt+PgDown**, configurable under **Keyboard → Shortcuts → KWin**.

Open **Configure…** from a tray menu, then use **Add rule → Detect window properties**. Match an application, class, instance, role or title. Matching is case-sensitive: selected conditions use AND; enabled rules use OR. Use role/title conditions to distinguish separate windows of one app. Changes take effect with **Apply/OK**; Cancel discards edits.

Each rule offers:

- **Startup:** No startup action, Pin without initially hiding, or Start in tray. Start in tray also applies to existing windows when a rule first matches.
- **Minimize to tray:** On, Off, or the global default. For minimize-only handling, choose No startup action and Minimize On.
- **Close to tray:** Uses the separately assigned **Close or hide window to tray** shortcut. It has no default binding and does not change Alt+F4. Titlebar/client close buttons, application Quit and tray Quit still close normally.
- **Retry early startup restoration once:** Off by default. With Start in tray, allows one retry within 1.5 seconds if the window reopens without activation. Show, Unpin, Close, activation, matching-property/configuration changes and unload cancel it. KWin cannot identify who requested a restore; non-activating user restores can also be retried. Disable it if it interferes.

Overlapping rules combine actions: Start in tray wins over pin-only; Minimize On wins over Off, then the global default; any matching rule can enable Close or startup retry.

**Show** prevents startup re-hiding; later minimize/close actions can hide again. **Unpin** suppresses automatic actions until the window stops matching all rules or is manually pinned again. Removing the last matching rule restores automatic tray-hidden windows; manual pins remain independent. Tray Quit closes only its associated window.

Existing settings migrate with backups on Apply; startup retry defaults Off. Invalid rules are reported and retained. **View as text** exports read-only JSON.

## Acknowledgements

[Unity LauncherAPI](https://wiki.ubuntu.com/Unity/LauncherAPI) implementation is a simplified version of [KDE/plasma-desktop/applets/taskmanager/plugin/smartlaunchers](https://github.com/KDE/plasma-desktop/tree/e3ba92b113d8a4e2d47a589835e9d867059dc2b9/applets/taskmanager/plugin/smartlaunchers)

## Support the original author

The links below support **Luis Bocanegra**, the original author of Minimize2Tray. If this project has been useful to you, consider sponsoring or donating to support his work on this project and [his other open source work](https://github.com/luisbocanegra?tab=repositories&q=&type=source&language=&sort=stargazers)

[![GitHub Sponsors](https://img.shields.io/badge/GitHub%20Sponsors-%2329313C?logo=githubsponsors&style=for-the-badge)](https://github.com/sponsors/luisbocanegra)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-supporter?logo=ko-fi&logoColor=%23ffffff&color=%23467BEB&style=for-the-badge)](https://ko-fi.com/luisbocanegra)
[!["Buy Me A Coffee"](https://img.shields.io/badge/Buy%20me%20a%20coffee-supporter?logo=buymeacoffee&logoColor=%23282828&color=%23FF803F&style=for-the-badge)](https://www.buymeacoffee.com/luisbocanegra)
[![PayPal](https://img.shields.io/badge/PayPal-supporter?logo=paypal&logoColor=%23ffffff&color=%23003087&style=for-the-badge)](https://www.paypal.com/donate/?hosted_button_id=Y5TMH3Z4YZRDA)
[![Patreon](https://img.shields.io/badge/Patreon-%23000000?logo=patreon&logoColor=%23ffffff&style=for-the-badge)](https://patreon.com/luisbocanegra?utm_medium=unknown&utm_source=join_link&utm_campaign=creatorshare_creator&utm_content=copyLink)
