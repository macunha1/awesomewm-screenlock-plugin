# AwesomeWM Screenlock Plugin

Locking a desktop should hide what was on the screen without making the
transition feel like a blank interruption. This plugin turns the last visible
desktop into a frozen, blurred image before the lock surface appears. The
result preserves privacy while keeping the lock screen tied to the session it
is protecting (and it avoids the visual jump of a generic static background).

This repository exists for AwesomeWM users who want that experience without
putting the sensitive parts of locking inside the window manager's Lua
process. It combines a small Lua-facing API with a native helper that owns the
lock while it is active. That separation makes the implementation easier to
reason about: configuration stays pleasant to use from `rc.lua`, while screen
capture, input, and authentication stay behind a narrower native boundary.

If you want an X11 locker that is visually considerate, PAM-backed, and
deliberate about where passwords and display access go, this is a repository
worth considering. It is inspired by
[`i3lock`](https://github.com/i3/i3lock), but shaped for AwesomeWM's LuaJIT
configuration model.

Licensed under the [MIT License](./LICENSE).

## What it provides

The Lua module starts `awesomewm-screenlock-helper` asynchronously. The helper
owns the X connection while the screen is locked, so an Awesome event-loop
failure does not leave the lock state inside the window manager process. The
Lua API remains responsible for configuration and lifecycle, while the helper
keeps the lock surface, password handling, and authentication independent of
Awesome's event loop.

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

The helper captures the desktop before mapping the lock surface and applies a
privacy filter that makes the previous desktop look frozen and blurred:

```text
noise=alls=10,scale=iw*.05:-1,scale=iw*20:-1:flags=neighbor
```

This gives every lock a dynamic image from the display that was actually in
use, while reducing readable content before the lock surface is shown. The
effect is practical rather than decorative: it preserves the context of the
session without leaving the previous desktop legible to someone looking over
your shoulder.

## Design boundary

Lua owns configuration and process lifecycle. C owns filtered capture, the
fullscreen XCB window, password input, and PAM authentication. The password
never crosses into Lua, which keeps AwesomeWM's scripting layer out of the
credential path and limits the native helper's responsibilities to the parts
that need direct access to the display and authentication stack.

This is an X11 locker, not a general-purpose desktop security boundary. It is
designed to protect a screen from ordinary nearby observation and to keep
credentials out of the window manager process. X11's trust model still
applies: an X11 client with equivalent authority can observe, inject input,
or otherwise interfere with any X11 locker. This plugin cannot provide
Wayland security guarantees, and it should not be presented as protection
against a compromised session, a hostile X11 client, or an attacker who
already has equivalent access to the display server.

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
