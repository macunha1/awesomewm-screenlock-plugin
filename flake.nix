{
  description = "AwesomeWM screenlock plugin with a Lua API and XCB/PAM helper";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.devenv = {
    url = "github:cachix/devenv/v2.4.0";
    inputs.nixpkgs.follows = "nixpkgs";
  };

  nixConfig = {
    extra-substituters = [ "https://devenv.cachix.org" ];
    extra-trusted-public-keys = [
      "devenv.cachix.org-1:w1cLUi8dv3hnoSPGAuibQv+f9TZLr6cv/Hm9XgU50cw="
    ];
  };

  outputs =
    {
      self,
      nixpkgs,
      devenv,
      ...
    }@inputs:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = nixpkgs.lib.genAttrs systems;
      nixosModule = import ./build/nix/nixos-module.nix { inherit self; };

      packageFor =
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        pkgs.callPackage ./build/nix/package.nix { src = self; };
    in
    {
      overlays.default = final: _previous: {
        awesomewm-screenlock-plugin = final.callPackage ./build/nix/package.nix { src = self; };
      };

      nixosModules = {
        default = nixosModule;
        awesomewm-screenlock = nixosModule;
      };

      packages = forEachSystem (system: {
        default = packageFor system;
        awesomewm-screenlock-plugin = packageFor system;
      });

      devShells = forEachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = devenv.lib.mkShell {
            inherit inputs pkgs;
            modules = [ ./devenv.nix ];
          };
        }
      );

      formatter = forEachSystem (system: nixpkgs.legacyPackages.${system}.nixfmt);

      checks = forEachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          package = packageFor system;
          nixosConfiguration = nixpkgs.lib.nixosSystem {
            inherit system;
            modules = [
              nixosModule
              {
                services.xserver.windowManager.awesome.enable = true;
                services.awesomewm-screenlock.enable = true;
                system.stateVersion = "26.05";
              }
            ];
          };
        in
        {
          build = package;
          nixos-module =
            nixpkgs.lib.findFirst (check: check.name == "awesomewm-screenlock-pam-compatibility")
              (throw "NixOS module did not add the PAM compatibility check")
              nixosConfiguration.config.system.checks;
          pam-compatibility = import ./build/nix/pam-compatibility-check.nix {
            inherit package pkgs;
          };
        }
      );
    };
}
