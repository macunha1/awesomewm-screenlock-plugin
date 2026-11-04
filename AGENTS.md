# AwesomeWM screenlock ownership

Keep passwords, grabs, capture, and PAM in the native helper. The Lua API starts
and observes it. Lockdown stays enabled by default. Finish privacy filtering
before mapping a lock surface. Integrated mode must repaint after restacking.

Meson is the compiler and test entry point. `devenv.nix` owns the shared task
graph; Actions YAML owns scheduling, caches, artifact transport, and publishing.
Use `nix develop --impure --no-update-lock-file --command devenv tasks run app:ci`.
Keep one `flake.lock`, with devenv following Nixpkgs. Do not add a second lock.
Preserve the NixOS host libc/PAM assertions and ELF compatibility checks.

Maintain LuaRocks metadata in `build/rockspec.lua`; generated `*.rockspec` files
are ignored. Package sources and verify installation from the resulting rock.
Publishing consumes the successful CI artifact and must not rebuild it, fetch
different sources, or receive credentials during validation. Keep external
actions pinned to commits. Keep setup, update commands, artifact contracts, and
lasting limitations in docs/build.md; execution logs belong in CI or review.
