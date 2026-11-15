find_program(MEINPROC6_EXECUTABLE NAMES meinproc6 REQUIRED)
set(HANDBOOK_CACHES)
foreach(language en bg)
    if(language STREQUAL "en")
        set(handbook "${CMAKE_CURRENT_SOURCE_DIR}/index.docbook")
    else()
        set(handbook "${CMAKE_CURRENT_SOURCE_DIR}/${language}/index.docbook")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${language}")
    set(cache "${CMAKE_CURRENT_BINARY_DIR}/${language}/index.cache.bz2")
    add_custom_command(OUTPUT "${cache}"
        COMMAND "${MEINPROC6_EXECUTABLE}" --check --cache "${cache}" "${handbook}"
        DEPENDS "${handbook}"
        VERBATIM)
    list(APPEND HANDBOOK_CACHES "${cache}")
    install(FILES "${handbook}" "${cache}" DESTINATION "${KDE_INSTALL_DOCBUNDLEDIR}/${language}/kwin-minimize2tray")
endforeach()
add_custom_target(minimize2tray-handbook ALL DEPENDS ${HANDBOOK_CACHES})
