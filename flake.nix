{
  description = "AwesomeWM screenlock plugin with a Lua API and XCB/PAM helper";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = nixpkgs.lib.genAttrs systems;

      packageFor = system:
        let
          pkgs = import nixpkgs { inherit system; };
          ffmpeg = pkgs.ffmpeg.override {
            withXcb = true;
            withXcbShm = true;
            withXcbShape = true;
            withXcbxfixes = true;
          };
        in
        pkgs.stdenv.mkDerivation {
          pname = "awesomewm-screenlock-plugin";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            luajit
          ];

          mesonFlags = [ "-Dlua-version=5.1" ];

          buildInputs = with pkgs; [
            libxcb
            libx11
            libxft
            xcbutilkeysyms
            pam
            ffmpeg
          ];

          passthru.luaModulePaths = [ "share/lua/5.1" ];
          passthru.luaModule = "share/lua/5.1";

          meta = {
            description = "AwesomeWM screenlock plugin with a Lua API and XCB/PAM helper";
            homepage = "https://github.com/macunha1/awesomewm-screenlock-plugin";
            license = pkgs.lib.licenses.mit;
            platforms = pkgs.lib.platforms.linux;
            mainProgram = "awesomewm-screenlock-helper";
          };
        };
    in
    {
      packages = forEachSystem (system: {
        default = packageFor system;
        awesomewm-screenlock-plugin = packageFor system;
      });

      devShells = forEachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          ffmpeg = pkgs.ffmpeg.override {
            withXcb = true;
            withXcbShm = true;
            withXcbShape = true;
            withXcbxfixes = true;
          };
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              meson
              ninja
              pkg-config
              luajit
            ];
            buildInputs = with pkgs; [
              libxcb
              libx11
              libxft
              xcbutilkeysyms
              pam
              ffmpeg
            ];
          };
        }
      );

      checks = forEachSystem (system: {
        build = packageFor system;
      });
    };
}
