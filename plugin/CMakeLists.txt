set(PLUGIN_NAME "trayiconplugin")
set(LIB_NAME "trayicon")

add_library(${PLUGIN_NAME} SHARED ${PLUGIN_NAME}.cpp ${LIB_NAME}.cpp scriptconfig.cpp rules.cpp sessiontermination.cpp)
find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS DBus Gui Qml Widgets)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS Config CoreAddons KCMUtils StatusNotifierItem Service XmlGui)

target_link_libraries(${PLUGIN_NAME} PRIVATE
    Qt6::Qml
    Qt6::DBus
    Qt6::Gui
    Qt6::Widgets
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::KCMUtils
    KF6::XmlGui
    KF6::StatusNotifierItem
    KF6::Service
    ${CMAKE_DL_LIBS}
)

install(TARGETS ${PLUGIN_NAME} DESTINATION ${KDE_INSTALL_QMLDIR}/com/github/luisbocanegra/${LIB_NAME})
install(FILES qmldir DESTINATION ${KDE_INSTALL_QMLDIR}/com/github/luisbocanegra/${LIB_NAME})

add_library(kcm_minimize2tray MODULE configmodule.cpp rules.cpp configbackup.cpp)
set_target_properties(kcm_minimize2tray PROPERTIES PREFIX "")
target_link_libraries(kcm_minimize2tray PRIVATE
    Qt6::Widgets Qt6::DBus KF6::KCMUtils KF6::ConfigCore KF6::CoreAddons KF6::Service
)
install(TARGETS kcm_minimize2tray DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/configs)

# Keep KWin's version-specific ABI out of the QML plugin. This optional adapter
# is inspected before loading and only used with the exact SDK version it targets.
option(BUILD_SESSION_TERMINATION "Build native Wayland logout termination support when the KWin SDK is available" ON)
if(BUILD_SESSION_TERMINATION)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/pidfd.h>
#include <sys/syscall.h>
int main() { pidfd_info info{}; unsigned long request = PIDFD_GET_INFO; int option = SO_PEERPIDFD; long signal = SYS_pidfd_send_signal; return request && option && signal && sizeof(info) ? 0 : 1; }"
        MINIMIZE2TRAY_HAVE_PIDFD_API)
    set(_minimize2tray_module_path "${CMAKE_MODULE_PATH}")
    find_package(KWin 6.8 QUIET)
    set(CMAKE_MODULE_PATH "${_minimize2tray_module_path}")
    if(KWin_FOUND AND MINIMIZE2TRAY_HAVE_PIDFD_API)
        find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Quick)
        find_package(KF6WindowSystem ${KF6_MIN_VERSION} REQUIRED)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sessionprocess-metadata.json" "{\"KWinVersion\": \"${KWin_VERSION}\"}\n")
        add_library(sessionprocess MODULE sessionprocess.cpp sessionpeer.cpp)
        set_target_properties(sessionprocess PROPERTIES PREFIX "")
        target_include_directories(sessionprocess PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(sessionprocess PRIVATE KWin::kwin)
        install(TARGETS sessionprocess DESTINATION ${KDE_INSTALL_QMLDIR}/com/github/luisbocanegra/${LIB_NAME})
    else()
        message(STATUS "KWin 6.8+ SDK or Linux 6.13+ process-handle headers unavailable; logout termination is disabled (other features are unchanged)")
    endif()
endif()

target_compile_definitions(${PLUGIN_NAME} PRIVATE MINIMIZE2TRAY_HAS_SESSION_PROCESS=$<TARGET_EXISTS:sessionprocess>)
target_compile_definitions(kcm_minimize2tray PRIVATE MINIMIZE2TRAY_HAS_SESSION_PROCESS=$<TARGET_EXISTS:sessionprocess>)
