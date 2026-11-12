"""Exercise the helper on an isolated X server without host PAM authentication."""

import ctypes
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time


def wait_until(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("timed out waiting for the lock surface")


def context_switches(process):
    for line in Path(f"/proc/{process.pid}/status").read_text().splitlines():
        if line.startswith("voluntary_ctxt_switches:"):
            return int(line.split()[1])
    raise AssertionError("missing process scheduling counters")


helper, xvfb, x11_library, source = sys.argv[1:]
x11 = ctypes.CDLL(x11_library)
pointer = ctypes.c_void_p
window = ctypes.c_ulong
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XOpenDisplay.restype = pointer
x11.XDefaultRootWindow.argtypes = [pointer]
x11.XDefaultRootWindow.restype = window
x11.XSetWindowBackground.argtypes = [pointer, window, ctypes.c_ulong]
x11.XCreateSimpleWindow.argtypes = [pointer, window, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
                                  ctypes.c_ulong, ctypes.c_ulong]
x11.XCreateSimpleWindow.restype = window
x11.XMapWindow.argtypes = [pointer, window]
x11.XUnmapWindow.argtypes = [pointer, window]
x11.XDestroyWindow.argtypes = [pointer, window]
x11.XSetInputFocus.argtypes = [pointer, window, ctypes.c_int, ctypes.c_ulong]
x11.XGrabKeyboard.argtypes = [pointer, window, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_ulong]
x11.XUngrabKeyboard.argtypes = [pointer, ctypes.c_ulong]
x11.XClearWindow.argtypes = [pointer, window]
x11.XSync.argtypes = [pointer, ctypes.c_int]
x11.XGetInputFocus.argtypes = [pointer, ctypes.POINTER(window), ctypes.POINTER(ctypes.c_int)]
x11.XGetImage.argtypes = [pointer, window, ctypes.c_int, ctypes.c_int,
                         ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
x11.XGetImage.restype = pointer
x11.XGetPixel.argtypes = [pointer, ctypes.c_int, ctypes.c_int]
x11.XGetPixel.restype = ctypes.c_ulong
x11.XDestroyImage.argtypes = [pointer]
x11.XCloseDisplay.argtypes = [pointer]

with tempfile.TemporaryDirectory(prefix="screenlock-display-") as directory:
    server = subprocess.Popen(
        [xvfb, "-displayfd", "1", "-screen", "0", "800x600x24", "-nolisten", "tcp"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    )
    locker = None
    display = None
    try:
        assert select.select([server.stdout], [], [], 10)[0], "Xvfb did not start"
        display_name = b":" + server.stdout.readline().strip()
        display = x11.XOpenDisplay(display_name)
        assert display, "cannot connect to isolated Xvfb"
        root = x11.XDefaultRootWindow(display)
        x11.XSetWindowBackground(display, root, 0xFF0000)
        x11.XClearWindow(display, root)

        def create_client():
            client = x11.XCreateSimpleWindow(display, root, 600, 400, 100, 100,
                                             0, 0, 0xFF0000)
            x11.XMapWindow(display, client)
            return client

        def input_focus():
            focused = window()
            revert_to = ctypes.c_int()
            x11.XGetInputFocus(display, ctypes.byref(focused), ctypes.byref(revert_to))
            return focused.value, revert_to.value

        previous_focus = create_client()
        x11.XSetInputFocus(display, previous_focus, 1, 0)  # RevertToPointerRoot, CurrentTime.
        x11.XSync(display, 0)
        environment = dict(os.environ, DISPLAY=display_name.decode())
        path = f"{directory}/control"
        locker = subprocess.Popen([helper, "--control-socket", path], env=environment)
        focus = window()
        revert = ctypes.c_int()

        def locked():
            assert locker.poll() is None, "helper exited before locking"
            x11.XGetInputFocus(display, ctypes.byref(focus), ctypes.byref(revert))
            return focus.value not in (0, 1, root, previous_focus)

        wait_until(locked)
        time.sleep(0.15)  # Allow the initial expose event to finish.
        before = context_switches(locker)
        time.sleep(0.3)
        wakeups = context_switches(locker) - before
        assert wakeups <= 2, f"idle helper woke {wakeups} times in 300 ms"

        def pixel():
            image = x11.XGetImage(display, focus, 16, 16, 1, 1, 0xFFFFFFFF, 2)
            assert image
            try:
                return x11.XGetPixel(image, 0, 0)
            finally:
                x11.XDestroyImage(image)

        # A solid red desktop remains red through the privacy filter. A valid
        # notification paints a black rectangle here, proving socket readiness
        # wakes the helper and reaches the renderer.
        assert pixel() & 0xFF0000, "filtered desktop was not rendered"
        environment["LUA_PATH"] = f"{source}/lua/?.lua;{source}/lua/?/init.lua;;"
        subprocess.run(["luajit", "-", path], input='''
package.preload.awful = function() return {} end
local screenlock = require("awesomewm_screenlock")({ controlSocket = arg[1] })
screenlock.active = true
assert(screenlock:notify("title", string.rep("x", 20000), "info"))
''', text=True, env=environment, check=True, timeout=5)
        wait_until(lambda: pixel() == 0)
        locker.terminate()
        assert locker.wait(timeout=2) == 1, "unexpected helper termination status"
        assert input_focus() == (previous_focus, 1), "previous client focus was not restored"
        x11.XDestroyWindow(display, previous_focus)

        # Both lock modes use the same teardown as successful authentication.
        # Include clients that disappear while locked and X11's special targets.
        wibar = create_client()
        for integrated in (False, True):
            command = [helper]
            if integrated:
                command += ["--integrated", "--wibar-window", str(wibar)]
            for target_state in ("viewable", "unmapped", "destroyed", "none", "pointer-root"):
                client = create_client()
                previous_focus = {"none": 0, "pointer-root": 1}.get(target_state, client)
                x11.XSetInputFocus(display, previous_focus, 2, 0)  # RevertToParent.
                x11.XSync(display, 0)
                saved_focus = input_focus()
                locker = subprocess.Popen(command, env=environment)
                wait_until(locked)
                if target_state == "unmapped":
                    x11.XUnmapWindow(display, client)
                elif target_state == "destroyed":
                    x11.XDestroyWindow(display, client)
                x11.XSync(display, 0)
                locker.terminate()
                assert locker.wait(timeout=2) == 1, "unexpected helper termination status"
                restored_focus = input_focus()
                if target_state in ("unmapped", "destroyed"):
                    assert restored_focus[0] in (0, 1, root), "focus restored to an unavailable client"
                else:
                    assert restored_focus == saved_focus, (integrated, target_state, restored_focus, saved_focus)
                if target_state != "destroyed":
                    x11.XDestroyWindow(display, client)

        # Failure to acquire a grab happens after moving focus to the lock.
        # It must restore the client too, without requiring an unlock callback.
        previous_focus = create_client()
        x11.XSetInputFocus(display, previous_focus, 1, 0)
        assert x11.XGrabKeyboard(display, previous_focus, 0, 1, 1, 0) == 0
        try:
            result = subprocess.run([helper], env=environment, timeout=5, check=False)
            assert result.returncode == 2, "helper should fail when the keyboard is already grabbed"
            assert input_focus() == (previous_focus, 1), "startup failure did not restore focus"
        finally:
            x11.XUngrabKeyboard(display, 0)
        print("isolated X11 lock, Lua notification, focus restoration, and signal exit passed; "
              f"idle wakeups: {wakeups}")
    finally:
        if locker is not None and locker.poll() is None:
            locker.kill()
            locker.wait()
        if display:
            x11.XCloseDisplay(display)
        server.terminate()
        server.wait(timeout=5)
