#!/usr/bin/env sh
set -eu

lua_bin=${LUA_BIN:-lua}
"$lua_bin" -e 'assert(loadfile("lua/awesomewm_screenlock/init.lua"))'
