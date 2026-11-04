#!/usr/bin/env sh
set -eu

lua_bin=${1:?Lua interpreter required}
source_dir=${2:?source directory required}
module_dir=${3:-$source_dir/lua}

LUA_PATH="$module_dir/?.lua;$module_dir/?/init.lua;;" \
    "$lua_bin" - <<'LUA'
local calls = {}
local pending_callback
local defer_exit = false

package.preload.awful = function()
    return {
        spawn = {
            easy_async = function(command, callback)
                calls[#calls + 1] = command
                if defer_exit then
                    pending_callback = callback
                else
                    callback("", "", "exit", 0)
                end
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

-- Keep one helper active until its asynchronous exit callback arrives.
defer_exit = true
screenlock:lock()
assert(screenlock.active == true)
screenlock:lock()
assert(#calls == 4)
assert(screenlock:notify("title", "text") == false)
pending_callback("", "", "exit", 0)
assert(screenlock.active == false)
assert(screenlock:notify("title", "text") == false)
defer_exit = false

-- Existing Awesome configurations keep the compatibility entry point.
require("plugins.screenlock").lock()
assert(calls[5][1] == "awesomewm-screenlock-helper")
LUA
