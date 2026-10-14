{ self }:

{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.awesomewm-screenlock;
  defaultPackage = pkgs.callPackage ./package.nix { src = self; };
  pamCompatibilityCheck = import ./pam-compatibility-check.nix {
    package = cfg.package;
    inherit pkgs;
  };
in
{
  options.services.awesomewm-screenlock = {
    enable = lib.mkEnableOption "the AwesomeWM screenlock plugin";

    package = lib.mkOption {
      type = lib.types.package;
      default = defaultPackage;
      defaultText = lib.literalExpression "pkgs.callPackage ./build/nix/package.nix { src = self; }";
      description = ''
        The awesomewm-screenlock-plugin package. The default is built with the
        NixOS host package set so its libc and PAM dependencies cannot drift
        from the PAM modules loaded during authentication.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = config.services.xserver.windowManager.awesome.enable;
        message = "services.awesomewm-screenlock requires AwesomeWM to be enabled";
      }
      {
        assertion =
          cfg.package ? stdenv
          && cfg.package.stdenv ? cc
          && cfg.package.stdenv.cc ? libc
          && cfg.package.stdenv.cc.libc == pkgs.stdenv.cc.libc;
        message = "services.awesomewm-screenlock.package must use the host nixpkgs libc";
      }
      {
        assertion = builtins.elem pkgs.pam (cfg.package.buildInputs or [ ]);
        message = "services.awesomewm-screenlock.package must use the host nixpkgs PAM package";
      }
    ];

    environment.systemPackages = [ cfg.package ];
    security.pam.services.xlock.enable = true;
    services.xserver.windowManager.awesome.luaModules = [ cfg.package ];
    system.checks = [ pamCompatibilityCheck ];
  };
}
