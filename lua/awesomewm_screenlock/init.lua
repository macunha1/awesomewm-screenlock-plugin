local awful = require("awful")
local ffi_available, ffi = pcall(require, "ffi")

if ffi_available then
    ffi.cdef [[
        int socket(int domain, int type, int protocol);
        int connect(int socket, const void *address, unsigned int address_length);
        long write(int file_descriptor, const void *buffer, unsigned long count);
        int close(int file_descriptor);
    ]]
end

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

local function normalize_lockdown(lock_down)
    lock_down = lock_down or {}
    return {
        -- Secure mode is the default. Disabling it deliberately leaves the
        -- real Awesome wibar above the helper and allows its interactions.
        enabled = lock_down.enabled ~= false,
        -- An empty list means all registered actions are allowed in integrated
        -- mode. Action-to-widget mapping is owned by the Awesome adapter.
        allowed_actions = lock_down.allowedActions or {},
    }
end

local function pack_string(value)
    value = tostring(value or "")
    local length = #value

    if length < 32 then
        return string.char(0xa0 + length) .. value
    elseif length < 256 then
        return string.char(0xd9, length) .. value
    elseif length < 65536 then
        return string.char(0xda, math.floor(length / 256), length % 256) .. value
    end
    return string.char(0xda, 0xff, 0xff) .. value:sub(1, 65535)
end

local function notification_payload(title, text, level)
    local fields = {
        pack_string("type") .. pack_string("notification"),
        pack_string("title") .. pack_string(title),
        pack_string("text") .. pack_string(text),
        pack_string("level") .. pack_string(level or "info"),
    }
    return string.char(0x80 + #fields) .. table.concat(fields)
end

local function send_control_message(path, payload)
    if not ffi_available or not path then
        return false
    end

    local socket = ffi.C.socket(1, 1, 0) -- AF_UNIX, SOCK_STREAM.
    if socket < 0 then
        return false
    end

    local address = ffi.new("uint8_t[110]")
    address[0] = 1 -- AF_UNIX, little-endian sa_family_t.
    address[1] = 0
    ffi.copy(address + 2, path, math.min(#path, 107))
    local frame = string.char(
        math.floor((#payload + 1) / 16777216) % 256,
        math.floor((#payload + 1) / 65536) % 256,
        math.floor((#payload + 1) / 256) % 256,
        (#payload + 1) % 256
    ) .. string.char(1) .. payload -- MessagePack control frame type 1.
    local buffer = ffi.new("uint8_t[?]", #frame)

    ffi.copy(buffer, frame, #frame)
    if ffi.C.connect(socket, address, 2 + #path + 1) ~= 0 then
        ffi.C.close(socket)
        return false
    end
    local written = ffi.C.write(socket, buffer, #frame)
    ffi.C.close(socket)
    return written == #frame
end

function Screenlock:new(args)
    args = args or {}
    return setmetatable({
        active = false,
        helper = args.helper or self.helper,
        lock_down = normalize_lockdown(args.lockDown),
        control_socket = args.controlSocket,
        wibar_windows = args.wibarWindows,
    }, { __index = self })
end

function Screenlock:lock()
    if self.active then
        return
    end

    self.active = true
    local command = { self.helper }

    if self.control_socket then
        table.insert(command, "--control-socket")
        table.insert(command, self.control_socket)
    end
    if not self.lock_down.enabled then
        table.insert(command, "--integrated")
        for _, window in ipairs(type(self.wibar_windows) == "function"
                                and self.wibar_windows() or self.wibar_windows or {}) do
            table.insert(command, "--wibar-window")
            table.insert(command, tostring(window))
        end
    end

    awful.spawn.easy_async(command, function(_, stderr, _, exit_code)
        self.active = false
        if exit_code ~= 0 then
            report_failure(stderr, exit_code)
        end
    end)
end

function Screenlock:notify(title, text, level)
    if not self.active then
        return false
    end
    if send_control_message(
            self.control_socket,
            notification_payload(title, text, level)
        ) then
        return true
    end
    return false
end

return setmetatable(Screenlock, { __call = Screenlock.new })
