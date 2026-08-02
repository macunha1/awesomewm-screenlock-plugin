local awful = require("awful")

local Screenlock = {
    active = false,
    helper = "awesomewm-screenlock-helper",
}

local function report_failure(stderr, exit_code)
    io.stderr:write(string.format(
        "awesomewm-screenlock: helper failed with exit code %d%s\n",
        exit_code,
        stderr and stderr ~= "" and (": " .. stderr) or ""
    ))
end

function Screenlock:new(args)
    args = args or {}
    return setmetatable({
        active = false,
        helper = args.helper or self.helper,
    }, { __index = self })
end

function Screenlock:lock()
    if self.active then
        return
    end

    self.active = true
    awful.spawn.easy_async({ self.helper }, function(_, stderr, _, exit_code)
        self.active = false
        if exit_code ~= 0 then
            report_failure(stderr, exit_code)
        end
    end)
end

return setmetatable(Screenlock, { __call = Screenlock.new })
