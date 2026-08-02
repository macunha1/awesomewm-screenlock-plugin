#!/usr/bin/env sh
set -eu

helper=${1:?helper path required}
test "$($helper invalid-argument 2>&1 >/dev/null || true)" != ""
