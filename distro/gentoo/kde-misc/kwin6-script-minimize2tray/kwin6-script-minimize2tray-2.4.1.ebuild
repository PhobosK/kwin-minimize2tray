# SPDX-License-Identifier: GPL-3.0-only

EAPI=8

inherit cmake multilib

DESCRIPTION="Minimize application windows to the tray in KDE Plasma 6 (PhobosK fork)"
HOMEPAGE="https://github.com/PhobosK/kwin-minimize2tray"
SRC_URI="${HOMEPAGE}/releases/download/v${PV}/kwin-minimize2tray-${PV}.tar.gz"
S="${WORKDIR}/kwin-minimize2tray-${PV}"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64"
IUSE="+native-plugin +script script-archive session-termination"
REQUIRED_USE="|| ( native-plugin script script-archive ) session-termination? ( native-plugin )"
RESTRICT="test"

RDEPEND="
	script? (
		>=kde-plasma/kwin-6.0:6
		>=kde-plasma/plasma-workspace-6.0:6
	)
	native-plugin? (
		>=kde-plasma/kwin-6.0:6
		>=kde-plasma/plasma-workspace-6.0:6
		>=dev-qt/qtbase-6.6:6[dbus,gui,widgets]
		>=dev-qt/qtdeclarative-6.6:6
		>=kde-frameworks/kconfig-6.0:6
		>=kde-frameworks/kcoreaddons-6.0:6
		>=kde-frameworks/kcmutils-6.0:6
		>=kde-frameworks/kservice-6.0:6
		>=kde-frameworks/kstatusnotifieritem-6.0:6
		>=kde-frameworks/kxmlgui-6.0:6
		>=kde-apps/khelpcenter-24.02:6
	)
	session-termination? (
		>=kde-plasma/kwin-6.8:6=
		>=kde-frameworks/kwindowsystem-6.0:6
	)
"
DEPEND="${RDEPEND}
	session-termination? (
		>=sys-kernel/linux-headers-6.13
		dev-util/vulkan-headers
	)
"
BDEPEND="
	native-plugin? ( >=kde-frameworks/kdoctools-6.0:6 )
	>=kde-frameworks/extra-cmake-modules-6.0
	session-termination? ( virtual/pkgconfig )
"
DOCS=( README.md CONTRIBUTING.md )

src_configure() {
	local mycmakeargs=(
		-DBUILD_PLUGIN="$(usex native-plugin)"
		-DINSTALL_SCRIPT="$(usex script)"
		-DPACKAGE_SCRIPT="$(usex script-archive)"
		-DBUILD_SESSION_TERMINATION="$(usex session-termination)"
		-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF
		-DMINIMIZE2TRAY_VERSION="${PV}"
		-DKDE_INSTALL_QMLDIR="$(get_libdir)/qt6/qml"
		-DKDE_INSTALL_PLUGINDIR="$(get_libdir)/qt6/plugins"
	)
	cmake_src_configure
}

src_install() {
	cmake_src_install
	if use script-archive; then
		insinto "/usr/share/${PN}"
		doins "${BUILD_DIR}/kwin-minimize2tray-v${PV}.kwinscript"
	fi
}
