# SPDX-License-Identifier: GPL-3.0-only
# Override the release with --define 'release_version 2.4.2'.
# To build a commit instead, also add --define 'commit FULL_SHA'.
%{!?release_version:%global release_version 2.4.2}

# Download the fixed archive when it is missing from SOURCES.
%global _disable_source_fetch 0

Name:           kwin6-script-minimize2tray
Version:        %{release_version}
# Bump Release when packaging another commit at the same version.
Release:        0.phobosk
Summary:        KDE Plasma 6 KWin script to minimize windows to the system tray
License:        GPL-3.0-only
Group:          System/GUI/KDE
URL:            https://github.com/PhobosK/kwin-minimize2tray
%if 0%{?commit:1}
Source0:        %{url}/archive/%{commit}.tar.gz#/%{name}-PhobosK-%{commit}.tar.gz
%else
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-PhobosK-v%{version}.tar.gz
%endif

BuildRequires:  cmake >= 3.19
BuildRequires:  kf6-kdoctools
BuildRequires:  gcc-c++
BuildRequires:  linux-glibc-devel >= 6.13
BuildRequires:  cmake(KWin) >= 6.8
BuildRequires:  kf6-extra-cmake-modules >= 6.0.0
BuildRequires:  cmake(Qt6DBus) >= 6.6.0
BuildRequires:  cmake(Qt6Gui) >= 6.6.0
BuildRequires:  cmake(Qt6Qml) >= 6.6.0
BuildRequires:  cmake(Qt6Widgets) >= 6.6.0
BuildRequires:  cmake(KF6Config) >= 6.0.0
BuildRequires:  cmake(KF6CoreAddons) >= 6.0.0
BuildRequires:  cmake(KF6KCMUtils) >= 6.0.0
BuildRequires:  cmake(KF6XmlGui) >= 6.0.0
BuildRequires:  cmake(KF6Service) >= 6.0.0
BuildRequires:  cmake(KF6StatusNotifierItem) >= 6.0.0
Requires:       khelpcenter
Requires:       kf6-kcmutils >= 6.0.0
Requires:       kwin6 >= 6.0.0
Requires:       plasma6-workspace >= 6.0.0

%description
Minimize2Tray is a KWin script with a native tray plugin and configuration
module. This package builds the PhobosK fork for KDE Plasma 6.

%prep
%if 0%{?commit:1}
%setup -q -n kwin-minimize2tray-%{commit}
%else
%setup -q -n kwin-minimize2tray-%{version}
%endif

%build
%cmake -DBUILD_PLUGIN=ON -DINSTALL_SCRIPT=ON -DPACKAGE_SCRIPT=OFF -DMINIMIZE2TRAY_VERSION:STRING=%{version} -DKDE_INSTALL_QMLDIR:PATH=%{_libdir}/qt6/qml -DKDE_INSTALL_PLUGINDIR:PATH=%{_libdir}/qt6/plugins
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md CONTRIBUTING.md
%{_datadir}/doc/HTML/en/kwin-minimize2tray/
%{_datadir}/kwin/scripts/com.github.luisbocanegra.minimize2tray/
%{_libdir}/qt6/qml/com/github/luisbocanegra/trayicon/
%{_libdir}/qt6/plugins/kwin/effects/configs/kcm_minimize2tray.so

%changelog
* Sat Nov 14 2026 PhobosK
- Release 2.4.2: simplify rule help and prevent accidental Match changes.
- Improve tray option ordering and handbook presentation.
* Sat Nov 14 2026 PhobosK
- Release 2.4.1: prepare a fresh tag for package builds.
* Sat Nov 14 2026 PhobosK
- Release 2.4.0: improve launcher icons, window rules and KDE help.
- Add icon overrides, global startup and safer window detection.
* Thu Nov 12 2026 PhobosK
- Release 2.3.0: bind package builds to the immutable workflow commit.
- Run existing-tag builds in their tag context.
* Wed Nov 11 2026 PhobosK
- Release 2.2.0: disable shared cache access in release jobs.
* Sun Nov 08 2026 PhobosK
- Release 2.1.0: add Gentoo and Arch Linux packaging and release targets.
* Sat Nov 07 2026 PhobosK
- Release 2.0.0: add native window picking for automatic tray rules.
- Add a separate tray action, unassigned shortcut, existing-rule choices,
  optional suggestions and post-creation review.
* Tue Nov 03 2026 PhobosK
- Release 1.0.0 with reliability, compatibility and packaging fixes.
* Mon Nov 02 2026 PhobosK
- Release 0.6.2: harden tray restoration and reduce compositor event overhead.
- Resolve rule application icons and persist sorting immediately.
* Sun Nov 01 2026 PhobosK
- Release 0.6.1: fix startup hiding, configuration stacking and rule sorting.
* Sun Nov 01 2026 PhobosK
- Update to release 0.6.0.
* Sun Nov 01 2026 PhobosK
- Simplify fork-only tagged release and commit builds.
* Sat Oct 24 2026 PhobosK
- Support release tags and pinned commits from the fork or upstream.
