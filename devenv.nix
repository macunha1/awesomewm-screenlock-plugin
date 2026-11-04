{ pkgs, ... }:

let
  package = pkgs.callPackage ./build/nix/package.nix { src = ./.; };
in
{
  languages.c.enable = true;
  languages.c.lsp.enable = false;
  languages.lua = {
    enable = true;
    package = pkgs.luajit;
    lsp.enable = false;
  };
  packages =
    package.nativeBuildInputs
    ++ package.buildInputs
    ++ [
      pkgs.luajitPackages.luarocks
      pkgs.nixfmt
      pkgs.unzip
      pkgs.zip
      pkgs.curl
      pkgs.python3
      pkgs.xorg-server
      pkgs.actionlint
      pkgs.shellcheck
    ];
  dotenv.enable = false;
  # Reuse the reviewed flake shell instead of evaluating a second environment.
  env.DEVENV_IN_DIRENV_SHELL = "true";
  env.FONTCONFIG_FILE = pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; };

  tasks = {
    "app:prepare" = {
      description = "Generate the ignored rockspec from maintained package metadata";
      exec = ''
        set -eu
        rock_package=$(luajit -e 'dofile("build/rockspec.lua"); io.write(package)')
        rockspec=$(luajit -e 'dofile("build/rockspec.lua"); io.write(package .. "-" .. version .. ".rockspec")')
        rm -f "$rock_package"-*.rockspec
        cp build/rockspec.lua "$rockspec"
      '';
    };
    "app:configure" = {
      description = "Configure the native Meson build";
      exec = "meson setup --reconfigure build/output --buildtype=release --libdir=lib -Dwerror=true";
    };
    "app:compile" = {
      description = "Compile the helper, library, and regression tests";
      after = [ "app:configure" ];
      exec = "meson compile -C build/output";
    };
    "app:test" = {
      description = "Run native and Lua regression tests";
      after = [ "app:compile" ];
      exec = "meson test -C build/output --no-rebuild --print-errorlogs";
    };
    "app:lint" = {
      description = "Check Nix formatting, shell scripts, Actions, and LuaRocks metadata";
      after = [ "app:prepare" ];
      exec = ''
        set -eu
        nixfmt --check flake.nix devenv.nix build/nix/*.nix
        luarocks lint ./*.rockspec
        actionlint .github/workflows/*.yml
        shellcheck build/*.sh tests/*.sh
      '';
    };
    "app:check" = {
      description = "Require all validation before packaging";
      after = [
        "app:test"
        "app:lint"
        "app:nix-check"
      ];
    };
    "app:nix-check" = {
      description = "Build the Nix package and verify host PAM compatibility";
      exec = "nix flake check --impure --no-update-lock-file";
    };
    "app:build" = {
      description = "Package the validated LuaRocks source artifact";
      after = [ "app:check" ];
      exec = "sh build/package.sh";
    };
    "app:verify" = {
      description = "Install and exercise the packaged LuaRocks artifact";
      after = [ "app:build" ];
      exec = "sh tests/test-rock.sh";
    };
    "app:release-test" = {
      description = "Test publish gates and artifact handoff without network uploads";
      after = [ "app:verify" ];
      exec = "python3 tests/test-release.py";
    };
    "app:ci" = {
      description = "Validate, package, and verify the distributable rock";
      after = [ "app:release-test" ];
      before = [ "devenv:enterTest" ];
      wantedBy = [ "devenv:enterTest" ];
    };
    "app:publish" = {
      description = "Publish an existing verified artifact to LuaRocks";
      exec = "sh build/publish.sh";
    };
  };
}
