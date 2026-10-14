{
  ffmpeg,
  lib,
  libx11,
  libxcb,
  libxft,
  luajit,
  meson,
  ninja,
  pam,
  pkg-config,
  src,
  stdenv,
  xcbutilkeysyms,
}:

let
  ffmpegWithX11 = ffmpeg.override {
    withXcb = true;
    withXcbShm = true;
    withXcbShape = true;
    withXcbxfixes = true;
  };
in
stdenv.mkDerivation {
  pname = "awesomewm-screenlock-plugin";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    inherit src;
    filter =
      path: type:
      let
        name = lib.baseNameOf path;
        relativePath = lib.removePrefix "${toString src}/" (toString path);
        isNixSource = relativePath == "build" || lib.hasPrefix "build/nix" relativePath;
      in
      isNixSource
      || (
        !lib.hasPrefix "build/" relativePath
        && !(type == "directory" && lib.hasPrefix "build" name)
        && name != "result"
      );
  };

  nativeBuildInputs = [
    luajit
    meson
    ninja
    pkg-config
  ];

  mesonFlags = [ "-Dlua-version=5.1" ];

  postPatch = ''
    patchShebangs tests
  '';

  buildInputs = [
    ffmpegWithX11
    libx11
    libxcb
    libxft
    pam
    xcbutilkeysyms
  ];

  doCheck = true;

  passthru.luaModulePaths = [ "share/lua/5.1" ];
  passthru.luaModule = "share/lua/5.1";

  meta = {
    description = "AwesomeWM screenlock plugin with a Lua API and XCB/PAM helper";
    homepage = "https://github.com/macunha1/awesomewm-screenlock-plugin";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "awesomewm-screenlock-helper";
  };
}
