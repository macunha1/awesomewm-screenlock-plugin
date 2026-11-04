#!/usr/bin/env sh
set -eu

rockspec=$(find . -maxdepth 1 -name '*.rockspec' -print)
test -f "$rockspec"
rock_version=$(luajit -e 'dofile(arg[0]); io.write(version)' "$rockspec")
version=${rock_version%-*}
source_dir="awesomewm-screenlock-plugin-$version"
name="awesomewm-screenlock-plugin-$rock_version"
test "$(sed -n "s/^  version: '\([^']*\)'.*/\1/p" meson.build)" = "$version"
if test -n "${RELEASE_TAG:-}"; then
    test "$RELEASE_TAG" = "v$version"
fi
luajit -e 'dofile(arg[0]); assert(source.dir == arg[1]);
    assert(source.url:match("/v" .. arg[2]:gsub("%.", "%%.") .. "%.tar%.gz$"))' \
    "$rockspec" "$source_dir" "$version"

mkdir -p dist
rm -f dist/*.rock dist/*.rockspec dist/SHA256SUMS
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
cp "$rockspec" "$stage/$name.rockspec"
# Package the reviewed working source, without fetching a possibly different
# tag or embedding a helper tied to the CI runner's libc and PAM paths.
tar -czf "$stage/$source_dir.tar.gz" --transform "s,^,$source_dir/," \
    LICENSE README.md meson.build meson_options.txt include src lua tests build/luarocks.sh build/rockspec.lua
(
    cd "$stage"
    zip -q "$name.src.rock" "$name.rockspec" "$source_dir.tar.gz"
)
cp "$stage/$name.rockspec" "$stage/$name.src.rock" dist/
(
    cd dist
    sha256sum ./*.rockspec ./*.src.rock > SHA256SUMS
)
