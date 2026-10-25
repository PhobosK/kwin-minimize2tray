#!/bin/sh

if [ -d "build" ]; then
    rm -rf build
fi

# Install widget and C++ plugin for current user
#
# NOTE: For the tray and configuration modules, add these environment variables
# in `$HOME/.config/plasma-workspace/env/path.sh`:
# export QML_IMPORT_PATH="$HOME/.local/lib64/qml:$HOME/.local/lib/qml:$QML_IMPORT_PATH"
# export QT_PLUGIN_PATH="$HOME/.local/lib64/plugins:$HOME/.local/lib/plugins:$QT_PLUGIN_PATH"
#
# For more information see https://userbase.kde.org/Session_Environment_Variables
cmake -B build/ -S . -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build/
cmake --install build/
