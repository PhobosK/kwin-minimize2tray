#
# spec file for package kwin6-script-minimize2tray
#
# SPDX-License-Identifier: GPL-3.0-only
#

%global upstream_version 0.1.0

# Immutable upstream Git revision.
%global commit f5e8d140a6660324cafa2188350c858134c80ab2
%global shortcommit %(echo %{commit} | cut -c1-7)

# Number of commits after the v0.1.0 tag.
# Increment/update this when moving to a newer upstream commit.
%global commit_offset 3

Name:           kwin6-script-minimize2tray
Version:        %{upstream_version}+git%{commit_offset}.%{shortcommit}
Release:        0.phobosk
Summary:        KDE Plasma 6 KWin script to minimize arbitrary windows to the system tray
License:        GPL-3.0-only
Group:          System/GUI/KDE
URL:            https://github.com/luisbocanegra/kwin-minimize2tray

# Pin the source archive to an immutable commit instead of the moving main branch.
Source0:        %{url}/archive/%{commit}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  extra-cmake-modules >= 6.0.0
BuildRequires:  gcc-c++
BuildRequires:  cmake(Qt6DBus) >= 6.6.0
BuildRequires:  cmake(Qt6Gui) >= 6.6.0
BuildRequires:  cmake(Qt6Qml) >= 6.6.0
BuildRequires:  cmake(Qt6Widgets) >= 6.6.0
BuildRequires:  cmake(KF6Package) >= 6.0.0
BuildRequires:  cmake(KF6Service) >= 6.0.0
BuildRequires:  cmake(KF6StatusNotifierItem) >= 6.0.0
Requires:       kwin6 >= 6.0.0
Requires:       plasma6-workspace >= 6.0.0

%description
Minimize2Tray is a KDE Plasma 6 KWin script plus QML plugin that can hide
minimizable windows from the normal task switcher, task manager and pager,
and expose restore and close actions through status notifier tray icons.

%prep
# GitHub may name commit archives using the resolved full SHA. Extracting into
# our own directory avoids depending on the archive's top-level directory name.
%setup -q -c -T
tar -xzf %{SOURCE0} --strip-components=1

%build
%cmake \
  -DBUILD_PLUGIN=ON \
  -DINSTALL_SCRIPT=ON \
  -DPACKAGE_SCRIPT=OFF

%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md CONTRIBUTING.md
%{_datadir}/kwin/scripts/com.github.luisbocanegra.minimize2tray/
%{_libdir}/qt6/qml/com/github/luisbocanegra/trayicon/

%changelog
