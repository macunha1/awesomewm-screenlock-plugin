# AwesomeWM Screenlock Plugin

An AwesomeWM screenlock plugin with a LuaJIT-facing Lua API and a separate C
helper for filtered X11 capture, XCB input grabs, and PAM authentication.

Licensed under the [MIT License](./LICENSE).

The Lua module starts `awesomewm-screenlock-helper` asynchronously. The helper
owns the X connection while the screen is locked, so an Awesome event-loop
failure does not leave the lock state inside the window manager process.

## Requirements

- AwesomeWM 4.x
- Lua or LuaJIT with `awful.spawn`
- X11 with XCB
- PAM configured with an `xlock` service
- FFmpeg libraries with the `x11grab` input and video filters

## Usage

Install the Lua module and helper, then add this to `rc.lua`:

```lua
local screenlock = require("awesomewm_screenlock")()

awful.key({ modkey }, "Home", function()
    screenlock:lock()
end)
```

The helper uses the `xlock` PAM service by default and reads
`AWESOMEWM_SCREENLOCK_PAM_SERVICE` when another service is required.

The helper captures the desktop before mapping the lock surface and applies the
privacy filter recovered from the former `bin/screenlock.sh` implementation:

```text
noise=alls=10,scale=iw*.05:-1,scale=iw*20:-1:flags=neighbor
```

The design is inspired by [`i3lock`](https://github.com/i3/i3lock): the native
helper owns the lock boundary, while the Lua module only starts and observes
its lifecycle.

## Design boundary

Lua owns configuration and process lifecycle. C owns filtered capture, the
fullscreen XCB window, password input, and PAM authentication. The password
never crosses into Lua.

This is an X11 locker. It is not a Wayland security boundary, and an X11
client with equivalent authority can interfere with any X11 locker.

## Build

```sh
meson setup build
meson compile -C build
meson test -C build
```

Nix users can build the package or development shell:

```sh
nix build
nix develop
```

The flake package contains both the helper binary and the Lua module, so a
configuration flake can expose one package to AwesomeWM:

```nix
inputs.awesomewm-screenlock-plugin.url =
  "github:macunha1/awesomewm-screenlock-plugin";

let
  screenlock = inputs.awesomewm-screenlock-plugin.packages.${pkgs.system}.default;
in
{
  environment.systemPackages = [ screenlock ];
  services.xserver.windowManager.awesome.luaModules = [ screenlock ];
}
```
