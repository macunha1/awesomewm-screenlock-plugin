#!/usr/bin/env sh
set -eu

root=$PWD
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
(
    cd dist
    sha256sum --check SHA256SUMS
)
set -- "$root"/dist/*.src.rock
test "$#" -eq 1
# Installing the source artifact compiles only its bundled source, in a fresh
# directory, and exercises its Meson tests before deployment to this tree.
luarocks --tree="$stage/tree" install --deps-mode=none "$1"
test -x "$stage/tree/bin/awesomewm-screenlock-helper"
sh tests/test-helper.sh "$stage/tree/bin/awesomewm-screenlock-helper"
sh tests/test-lua.sh luajit "$stage/tree/share" "$stage/tree/share/lua/5.1"
test -f "$stage/tree/share/lua/5.1/plugins/screenlock.lua"
