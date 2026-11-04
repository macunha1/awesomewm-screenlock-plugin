#!/usr/bin/env sh
set -eu

: "${LUAROCKS_API_KEY:?Set the luarocks environment secret LUAROCKS_API_KEY}"
: "${RELEASE_TAG:?Publishing requires a version tag}"
set -- dist/*.rockspec
test "$#" -eq 1
rockspec=$1
rock_version=$(luajit -e 'dofile(arg[0]); io.write(version)' "$rockspec")
test "$RELEASE_TAG" = "v${rock_version%-*}"
source_rock="${rockspec%.rockspec}.src.rock"
test -f "$source_rock"
(
    cd dist
    sha256sum --check SHA256SUMS
)
# Supply the successful CI artifact explicitly so upload never fetches,
# rebuilds, or repacks it. --temp-key avoids saving the credential to disk.
luarocks upload --temp-key "$LUAROCKS_API_KEY" "$rockspec" "$source_rock" </dev/null
