# AwesomeWM screenlock build and LuaRocks releases

Nix owns the compiler, LuaJIT, LuaRocks, Meson, native libraries, and devenv.
`flake.lock` is the single input lock. Meson owns compilation, installation,
and regression tests. Native devenv tasks own ordering and artifact verification;
GitHub Actions owns triggers, caches, artifact transport, and publishing.

## Develop and validate

Install Nix with flakes enabled, review the cache settings in `flake.nix`, then run:

```sh
nix develop --impure --no-update-lock-file
devenv tasks run app:ci
```

The one-command equivalent used by CI is:

```sh
nix develop --impure --no-update-lock-file --command devenv tasks run app:ci
```

Shell entry performs no application builds. `app:prepare` generates the ignored
rockspec from `build/rockspec.lua`. `app:configure`, `app:compile`, and `app:test`
exercise the native and Lua code. `app:lint` checks Nix formatting and the
rockspec, while `app:nix-check` builds the Nix package and PAM compatibility
checks. `app:check` is the validation barrier before `app:build` packages sources.
`app:verify` installs that source rock into a temporary tree and exercises its
installed helper and both Lua entry points. `app:release-test` verifies the
credential, version, checksum, and artifact handoff gates with a mock uploader.
`app:ci` selects this complete graph,
as does `devenv test` through the flake integration.

The pinned environment includes Xvfb and a fixed fontconfig setup. Its display
test uses an isolated server to check filtered rendering, idle wakeups, Lua
notification delivery, and signal exit without touching the active desktop or
authenticating through the host PAM service. Direct Meson builds run this test
when Xvfb is available.

Meson remains usable directly for native development:

```sh
meson setup build/output
meson compile -C build/output
meson test -C build/output --print-errorlogs
```

The flake declares x86_64-linux and aarch64-linux. GitHub CI runs x86_64-linux.
PAM authentication and real desktop behavior still require testing against the
destination system's PAM configuration and display layout.

## Install through LuaRocks

After a release is published:

```sh
luarocks install awesomewm-screenlock-plugin
```

For a local checkout:

```sh
devenv tasks run app:prepare
luarocks make awesomewm-screenlock-plugin-0.1.0-1.rockspec
```

The destination needs a C compiler, Meson >= 1.6, Ninja, pkg-config, Lua 5.1
(including LuaJIT) or 5.2, and development libraries for PAM, X11, Xft, XCB
(including keysyms, Xinerama, and RandR), and FFmpeg (avcodec, avdevice, avfilter,
avformat, avutil, swscale, with x11grab and the noise/scale filters enabled).
Meson reports missing native dependencies during configuration. There are no
third-party Lua application dependencies to resolve or lock.

LuaRocks builds the project's C library into the helper executable and installs
the executable plus both Lua modules. System libraries remain dynamically
linked. Ensure the LuaRocks tree's `bin` is on Awesome's PATH and its Lua paths
are available to Awesome (e.g. using `luarocks path`). Configure the `xlock` PAM
service separately; installation does not change system authentication policy.
NixOS users should prefer the host-built module described in the README.

## Artifact and publishing contract

`dist/` contains one versioned `.rockspec`, the matching `.src.rock`, and
`SHA256SUMS`. The source rock bundles the reviewed working sources and rockspec;
building it does not fetch a release tag or include generated native binaries.
CI verifies installation from this archive before uploading the directory as
the `luarocks-source` Actions artifact. A source rock lets LuaRocks compile the
helper against the destination's libc and PAM instead of distributing Nix store
paths from a disposable runner.

Create a GitHub environment named `luarocks`, restrict it to release tags, and
set its `LUAROCKS_API_KEY` secret to an upload key for the LuaRocks package owner.
The key exists only in the publish step; validation and packaging do not need it.
If the LuaRocks account requires an interactive second factor for this key,
configure suitable unattended publishing credentials before enabling releases.

To release, update the Meson and Nix package versions together, update the
version, archive URL, and source directory in `build/rockspec.lua`, and run
`app:ci`. Push a tag matching `v<version>` (e.g. `v0.1.0`) on the reviewed commit.
The task graph rejects a tag that disagrees with package metadata. The publish
job runs only after CI succeeds, downloads the successful artifact, checks its
hashes and version, then calls `luarocks upload` with that explicit source rock.
It does not rebuild, repack, or overwrite an existing rock revision.

Published releases remain available. Correct mistakes with a new package
version and matching release tag; do not replace an artifact
that consumers may already have installed. Reverting the workflow stops future
uploads but does not retract previous releases.

## Focus restoration

The native helper saves the X11 keyboard focus before mapping the lock surface
and restores it after releasing input grabs and destroying the surface. Both
lockdown and integrated mode restore the previous window without requiring
pointer movement. The same cleanup applies when helper startup fails after
taking focus or the helper handles a termination signal.

X11 has one keyboard focus across all monitors. Awesome's per-screen client
history stays with Awesome; the helper restores the single active focus target
and its X11 reversion policy. If that window closes or becomes unviewable while
locked, restoration is skipped by the X server. The helper does not select a
replacement window or switch tags. Abrupt termination (e.g. `SIGKILL`) cannot
run this cleanup.

## Toolchain updates and caches

Update inputs explicitly with `nix flake update`, review `flake.lock`, and rerun
the complete graph. Routine commands use `--no-update-lock-file`. Keep devenv's
Nixpkgs input following the repository's input. Generate rockspecs from the
maintained Lua metadata; `*.rockspec`, `dist/`, and build output are ignored.

The [devenv flake integration](https://devenv.sh/guides/using-with-flakes/) supplies
the reviewed public cache and signing key. The GitHub setup action pins both the
Nix installer action and Nix bootstrap version, accepts these flake settings on
its disposable runner, and restores/saves the Nix store through GitHub's cache.
Keys share the platform, bootstrap configuration, lock, and environment inputs
across CI and publishing. The CI shell profile keeps the runtime closure alive
through post-job cache saving. Cache hits never skip validation. GitHub cache
scope, quota, eviction, and upstream availability can still cause cold builds.

This pins tooling and dependency resolution; working-tree builds inherit their
caller's environment and archive timestamps. It does not promise hermetic tasks
or byte-identical rocks. Dotenv loading is disabled. Remote cache restore/save
behavior must be measured from Actions logs (a warm local Nix store cannot prove
it). Hosted runner images remain mutable despite the pinned bootstrap.
