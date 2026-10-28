# SPDX-License-Identifier: GPL-3.0-only
# Build the latest release: rpmbuild -ba kwin6-script-minimize2tray.spec
# Original project: add --with upstream.
# Offline/fixed release: add --define 'tag v0.1.0'.
# Fixed commit: add --define 'commit FULL_SHA' --define 'upstream_version 0.1.0'.

%bcond_with upstream
%if %{with upstream}
%global source_owner luisbocanegra
%else
%global source_owner PhobosK
%endif

# git is needed before parsing when neither tag nor upstream_version is given.
# Query remote refs only; never clone, update or otherwise touch a checkout.
%{lua:
local tag = rpm.expand("%{?tag}")
local version = rpm.expand("%{?upstream_version}")
local commit = rpm.expand("%{?commit}")
if tag == "" and version ~= "" then tag = "v" .. version end
if tag == "" and commit ~= "" then error("A fixed commit requires upstream_version.") end
if tag == "" then
    local owner = rpm.expand("%{source_owner}")
    local command = "GIT_TERMINAL_PROMPT=0 timeout 30s git ls-remote --tags --refs https://github.com/" .. owner .. "/kwin-minimize2tray.git"
    local refs = assert(io.popen(command, "r"))
    local best
    for line in refs:lines() do
        local name = line:match("^[0-9a-fA-F]+%s+refs/tags/(.+)$")
        local candidate = name and name:gsub("^v", "")
        if candidate and candidate:match("^%d+%.%d+%.%d+$") then
            local order = best and rpm.vercmp(candidate, best) or 1
            if order > 0 or (order == 0 and name > tag) then
                tag, best = name, candidate
            end
        end
    end
    local ok = refs:close()
    if not ok then error("Cannot read tags from " .. owner .. "/kwin-minimize2tray. Retry online or define tag explicitly.") end
    if tag == "" then error("No stable X.Y.Z/vX.Y.Z tag found. Define tag and upstream_version explicitly.") end
end
if not tag:match("^[A-Za-z0-9][A-Za-z0-9._+-]*$") then error("Invalid tag name.") end
if version == "" then version = tag:gsub("^v", "") end
if not version:match("^%d+%.%d+%.%d+$") then error("Set upstream_version to an X.Y.Z release version.") end
if commit ~= "" and (not commit:match("^[0-9a-fA-F]+$") or #commit ~= 40) then error("commit must be a full 40-digit SHA.") end
rpm.define("tag " .. tag)
rpm.define("upstream_version " .. version)
}

%if "%{?commit}" != ""
%global source_ref %{commit}
%global archive_ref %{commit}
%else
%global source_ref refs/tags/%{tag}
%global archive_ref %{tag}
%endif

# Let rpmbuild fetch the HTTPS source archive automatically when it is missing.
%global _disable_source_fetch 0

Name:           kwin6-script-minimize2tray
Version:        %{upstream_version}
Release:        0.phobosk
Summary:        KDE Plasma 6 KWin script to minimize arbitrary windows to the system tray
License:        GPL-3.0-only
Group:          System/GUI/KDE
URL:            https://github.com/%{source_owner}/kwin-minimize2tray
Source0:        %{url}/archive/%{source_ref}.tar.gz#/%{name}-%{source_owner}-%{archive_ref}.tar.gz

BuildRequires:  cmake >= 3.19
BuildRequires:  gcc-c++
BuildRequires:  git-core
BuildRequires:  kf6-extra-cmake-modules >= 6.0.0
BuildRequires:  cmake(Qt6DBus) >= 6.6.0
BuildRequires:  cmake(Qt6Gui) >= 6.6.0
BuildRequires:  cmake(Qt6Qml) >= 6.6.0
BuildRequires:  cmake(Qt6Widgets) >= 6.6.0
BuildRequires:  cmake(KF6Config) >= 6.0.0
BuildRequires:  cmake(KF6CoreAddons) >= 6.0.0
# Older releases use KPackage; newer sources also provide the native KCM.
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
%setup -q -c -T
tar -xzf "%{SOURCE0}" --strip-components=1 --no-same-owner --no-same-permissions
# Older upstream CMake reads the application version from source metadata.
sed -i -E 's/("Version"[[:space:]]*:[[:space:]]*")[^"]*/\1%{upstream_version}/' package/metadata.json

%build
set --
if grep -q MINIMIZE2TRAY_VERSION CMakeLists.txt; then
    set -- "-DMINIMIZE2TRAY_VERSION:STRING=%{upstream_version}"
fi
%cmake "$@" -DBUILD_PLUGIN=ON -DINSTALL_SCRIPT=ON -DPACKAGE_SCRIPT=OFF -DKDE_INSTALL_QMLDIR:PATH=%{_libdir}/qt6/qml -DKDE_INSTALL_PLUGINDIR:PATH=%{_libdir}/qt6/plugins
%cmake_build

%install
%cmake_install
# Keep the same spec usable with releases that predate the native KCM.
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
