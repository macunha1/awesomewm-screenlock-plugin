#!/usr/bin/env sh
set -eu

lua_version=${1:?Lua version required}
# Embed this project's library in the helper. LuaRocks relocates executables
# into the tree's bin directory, independently of its versioned rock directory.
meson setup --reconfigure build/luarocks --buildtype=release \
    -Ddefault_library=static -Dlua-version="$lua_version"
meson compile -C build/luarocks
meson test -C build/luarocks --no-rebuild --print-errorlogs
