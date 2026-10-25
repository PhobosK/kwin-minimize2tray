set(PLUGIN_NAME "trayiconplugin")
set(LIB_NAME "trayicon")

add_library(${PLUGIN_NAME} SHARED ${PLUGIN_NAME}.cpp ${LIB_NAME}.cpp scriptconfig.cpp rules.cpp)
find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS DBus Gui Qml Widgets)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS Config CoreAddons StatusNotifierItem Service)

target_link_libraries(${PLUGIN_NAME} PRIVATE
    Qt6::Qml
    Qt6::DBus
    Qt6::Gui
    Qt6::Widgets
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::StatusNotifierItem
    KF6::Service
)

install(TARGETS ${PLUGIN_NAME} DESTINATION ${KDE_INSTALL_QMLDIR}/com/github/luisbocanegra/${LIB_NAME})
install(FILES qmldir DESTINATION ${KDE_INSTALL_QMLDIR}/com/github/luisbocanegra/${LIB_NAME})

find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS KCMUtils)
add_library(kcm_minimize2tray MODULE configmodule.cpp rules.cpp)
set_target_properties(kcm_minimize2tray PROPERTIES PREFIX "")
target_link_libraries(kcm_minimize2tray PRIVATE
    Qt6::Widgets Qt6::DBus KF6::KCMUtils KF6::ConfigCore KF6::CoreAddons KF6::Service
)
install(TARGETS kcm_minimize2tray DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/configs)
