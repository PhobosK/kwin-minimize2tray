find_program(MEINPROC6_EXECUTABLE NAMES meinproc6 REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/index.cache.bz2"
    COMMAND "${MEINPROC6_EXECUTABLE}" --check --cache "${CMAKE_CURRENT_BINARY_DIR}/index.cache.bz2" "${CMAKE_CURRENT_SOURCE_DIR}/index.docbook"
    DEPENDS index.docbook
    VERBATIM)
add_custom_target(minimize2tray-handbook ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/index.cache.bz2")
install(FILES index.docbook "${CMAKE_CURRENT_BINARY_DIR}/index.cache.bz2"
    DESTINATION "${KDE_INSTALL_DOCBUNDLEDIR}/en/kwin-minimize2tray")
