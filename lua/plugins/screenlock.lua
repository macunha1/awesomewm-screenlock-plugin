-- Compatibility entry point for Aweswm configurations that already require
-- plugins.screenlock. The standalone module owns the lock process and keeps
-- the old callback shape so existing AwesomeWM keybindings remain usable.
local screenlock = require("awesomewm_screenlock")()

return {
    lock = function()
        screenlock:lock()
    end,
}
