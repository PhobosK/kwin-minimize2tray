#
# spec file for package kwin6-script-minimize2tray
#
# SPDX-License-Identifier: GPL-3.0-only
#

# Use --with upstream to build the original repository.
%bcond_with upstream

# Release version shared by the RPM and About metadata.
%{!?upstream_version:%global upstream_version 0.4.0}
%{!?tag:%global tag v%{upstream_version}}

# Set commit to a full SHA to use a fixed revision instead of the release tag.
%{!?commit:%global commit %{nil}}

%if %{with upstream}
%global source_owner luisbocanegra
%else
%global source_owner PhobosK
%endif

%if "%{commit}" != ""
%global source_ref %{commit}
%global archive_ref %{commit}
%else
%global source_ref refs/tags/%{tag}
%global archive_ref %{tag}
%endif

Name:           kwin6-script-minimize2tray
Version:        %{upstream_version}
# Bump Release when packaging another commit at the same version.
Release:        0.phobosk
Summary:        KDE Plasma 6 KWin script to minimize arbitrary windows to the system tray
License:        GPL-3.0-only
Group:          System/GUI/KDE
URL:            https://github.com/%{source_owner}/kwin-minimize2tray

# Keep downloaded archives distinct when changing repository or revision.
Source0:        %{url}/archive/%{source_ref}.tar.gz#/%{name}-%{source_owner}-%{archive_ref}.tar.gz

BuildRequires:  cmake >= 3.19
BuildRequires:  gcc-c++
BuildRequires:  kf6-extra-cmake-modules >= 6.0.0
BuildRequires:  cmake(Qt6DBus) >= 6.6.0
BuildRequires:  cmake(Qt6Gui) >= 6.6.0
BuildRequires:  cmake(Qt6Qml) >= 6.6.0
BuildRequires:  cmake(Qt6Widgets) >= 6.6.0
# Required by the original build system.
BuildRequires:  cmake(KF6Package) >= 6.0.0
BuildRequires:  cmake(KF6KCMUtils) >= 6.0.0
BuildRequires:  cmake(KF6XmlGui) >= 6.0.0
BuildRequires:  cmake(KF6Service) >= 6.0.0
BuildRequires:  cmake(KF6StatusNotifierItem) >= 6.0.0
Requires:       kwin6 >= 6.0.0
Requires:       plasma6-workspace >= 6.0.0

%description
Minimize2Tray is a KDE Plasma 6 KWin script plus QML plugin that can hide
windows from the task switcher, task manager and pager, and expose restore
and close actions through status notifier tray icons.

%prep
# Tag and commit archives have different top-level directory names.
%setup -q -c -T
tar -xzf "%{SOURCE0}" --strip-components=1

# Older upstream sources read the About version directly from metadata.
sed -i -E 's/("Version"[[:space:]]*:[[:space:]]*")[^"]*/\1%{upstream_version}/' package/metadata.json

%build
%cmake \
  -DMINIMIZE2TRAY_VERSION:STRING="%{upstream_version}" \
  -DKDE_INSTALL_QMLDIR:PATH=%{_libdir}/qt6/qml \
  -DBUILD_PLUGIN=ON \
  -DINSTALL_SCRIPT=ON \
  -DPACKAGE_SCRIPT=OFF

%cmake_build

%install
%cmake_install
# Older fork/upstream archives do not ship the native configuration module.
: > minimize2tray-config.files
if test -f "%{buildroot}%{_libdir}/qt6/plugins/kwin/effects/configs/kcm_minimize2tray.so"; then
    echo "%{_libdir}/qt6/plugins/kwin/effects/configs/kcm_minimize2tray.so" > minimize2tray-config.files
fi

%files -f minimize2tray-config.files
%license LICENSE
%doc README.md CONTRIBUTING.md
%{_datadir}/kwin/scripts/com.github.luisbocanegra.minimize2tray/
%{_libdir}/qt6/qml/com/github/luisbocanegra/trayicon/

%changelog
* Sat Oct 24 2026 PhobosK
- Support release tags and pinned commits from the fork or upstream.
