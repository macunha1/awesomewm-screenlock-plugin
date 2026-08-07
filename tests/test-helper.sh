#!/usr/bin/env sh
set -eu

helper=${1:?helper path required}
test "$($helper invalid-argument 2>&1 >/dev/null || true)" != ""

# Integrated argument parsing must terminate before X11 initialization. A
# parser regression previously left the helper in a 100% CPU loop here.
if command -v timeout >/dev/null 2>&1; then
    DISPLAY= timeout 2 "$helper" --integrated --wibar-window 1 \
        >/dev/null 2>&1 || status=$?
    test "${status:-0}" -ne 124
fi
