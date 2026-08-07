#!/usr/bin/env sh
set -eu

lua_bin=${1:?Lua interpreter required}
source_dir=${2:?source directory required}

LUA_PATH="$source_dir/lua/?.lua;$source_dir/lua/?/init.lua;;" \
    "$lua_bin" - <<'LUA'
local calls = {}

package.preload.awful = function()
    return {
        spawn = {
            easy_async = function(command, callback)
                calls[#calls + 1] = command
                callback("", "", "exit", 0)
            end,
        },
    }
end

local screenlock = require("awesomewm_screenlock")({ helper = "test-helper" })
assert(screenlock.lock_down.enabled == true)
screenlock:lock()
assert(calls[1][1] == "test-helper")
assert(screenlock.active == false)
screenlock:lock()
assert(#calls == 2)

local integrated = require("awesomewm_screenlock")({
    helper = "test-helper",
    lockDown = { enabled = false, allowedActions = {} },
    wibarWindows = function() return { 1234 } end,
})
integrated:lock()
assert(calls[3][1] == "test-helper")
assert(calls[3][2] == "--integrated")
assert(calls[3][3] == "--wibar-window")
assert(calls[3][4] == "1234")
LUA
