cmake_minimum_required(VERSION 3.19)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -B build -S .")
endif()

option(CMAKE_EXPORT_COMPILE_COMMANDS "Generate compile_commands.json" ON)
project(kwin-minimize2tray LANGUAGES CXX)

# Set a version here, or leave empty to use Git or metadata. -D takes precedence.
if(NOT MINIMIZE2TRAY_VERSION)
    set(MINIMIZE2TRAY_VERSION "")
endif()

option(BUILD_PLUGIN "Build the plugin" ON)
option(INSTALL_SCRIPT "Install KWin Script" ON)
option(PACKAGE_SCRIPT "Package KWin Script" OFF)

set(QT_MIN_VERSION "6.6.0")
set(KF6_MIN_VERSION "6.0.0")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)

# Get id and version from metadata, using the Git tag when available.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/package/metadata.json" METADATA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/package/metadata.json")
string(JSON PLUGIN_ID GET "${METADATA}" KPlugin Id)
if(NOT MINIMIZE2TRAY_VERSION AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*.[0-9]*.[0-9]*" --match "[0-9]*.[0-9]*.[0-9]*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_VARIABLE MINIMIZE2TRAY_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_RESULT EQUAL 0)
            string(REGEX REPLACE "^v" "" MINIMIZE2TRAY_VERSION "${MINIMIZE2TRAY_VERSION}")
        else()
            set(MINIMIZE2TRAY_VERSION "")
        endif()
    endif()
endif()
if(NOT MINIMIZE2TRAY_VERSION)
    string(JSON MINIMIZE2TRAY_VERSION GET "${METADATA}" KPlugin Version)
endif()
if(NOT MINIMIZE2TRAY_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-+][A-Za-z0-9][A-Za-z0-9.+-]*)?$")
    message(FATAL_ERROR "Invalid version: '${MINIMIZE2TRAY_VERSION}'")
endif()

# Install generated metadata; keep the source version as the archive fallback.
string(JSON METADATA SET "${METADATA}" KPlugin Version "\"${MINIMIZE2TRAY_VERSION}\"")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/metadata.json" "${METADATA}\n")
message(STATUS "Plugin Id: ${PLUGIN_ID}")
message(STATUS "Version: ${MINIMIZE2TRAY_VERSION}")

if(INSTALL_SCRIPT)
    set(SCRIPT_INSTALL_DIR "${KDE_INSTALL_DATADIR}/kwin/scripts/${PLUGIN_ID}")
    install(DIRECTORY package/contents DESTINATION "${SCRIPT_INSTALL_DIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/metadata.json"
        DESTINATION "${SCRIPT_INSTALL_DIR}"
    )
endif()

if(PACKAGE_SCRIPT)
    set(SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-v${MINIMIZE2TRAY_VERSION}.kwinscript")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package")
    add_custom_target(kwinscript ALL
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/package/contents"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/contents" "${CMAKE_CURRENT_BINARY_DIR}/package/contents"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/metadata.json" "${CMAKE_CURRENT_BINARY_DIR}/package/metadata.json"
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${SCRIPT_FILE}" --format=zip metadata.json contents
        BYPRODUCTS "${SCRIPT_FILE}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package"
        COMMENT "Generating ${SCRIPT_FILE}"
        VERBATIM
    )
endif()

if(BUILD_PLUGIN)
    include(KDECMakeSettings)
    add_subdirectory(plugin)
endif()
