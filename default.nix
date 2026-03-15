# Builds the pyrotek45/ardour test/all-fixes branch from GitHub.
# Not tracked by git (see .gitignore).
#
# Install:   nix-env -f default.nix -i
# Reinstall: nix-env -f default.nix -i  (after updating rev/hash below)
# Uninstall: nix-env -e ardour-pk2

{
  pkgs ? import <nixpkgs> {},
  optimize ? true,
  videoSupport ? false,
}:

let
  inherit (pkgs) lib stdenv;
  ardourPkgDir = "${pkgs.path}/pkgs/applications/audio/ardour";
in

stdenv.mkDerivation rec {
  pname   = "ardour-pk2";
  version = "9.3-test-all-fixes";

  src = pkgs.fetchgit {
    url    = "https://github.com/pyrotek45/ardour";
    rev    = "ecd6b3deb1003c68cb169596741fa2fb0665f9b8";
    hash   = "sha256-LART7Lzk/7GbdiICcAEFHjNq3gv0tjlofJwz5l6GKFE=";
  };

  patches = [
    "${ardourPkgDir}/as-flags.patch"
    "${ardourPkgDir}/default-plugin-search-paths.patch"
  ];

  postPatch = ''
    printf '#include "libs/ardour/ardour/revision.h"\nnamespace ARDOUR { const char* revision = "${version}"; const char* date = ""; }\n' \
      > libs/ardour/revision.cc
    patchShebangs ./tools/
  '';

  nativeBuildInputs = with pkgs; [
    pkg-config
    python3
    itstool
    makeWrapper
    perl
    wafHook
    doxygen
    graphviz
  ];

  buildInputs = with pkgs; [
    alsa-lib
    aubio
    boost
    cairomm
    cppunit
    curl
    dbus
    ffmpeg
    fftw
    fftwSinglePrec
    flac
    fluidsynth
    glibmm
    hidapi
    kissfft
    libarchive
    libjack2
    liblo
    libltc
    libogg
    libpulseaudio
    librdf_rasqal
    libsamplerate
    libsigcxx
    libsndfile
    libusb1
    libuv
    libwebsockets
    xorg.libXi
    xorg.libXinerama
    xorg.libXrandr
    libxml2
    libxslt
    lilv
    lrdf
    lv2
    pango
    pangomm
    qm-dsp
    readline
    rubberband
    serd
    sord
    soundtouch
    sratom
    suil
    taglib
    vamp-plugin-sdk
  ] ++ lib.optionals videoSupport (with pkgs; [ harvid xjadeo ]);

  wafConfigureFlags = [
    "--cxx17"
    "--freedesktop"
    "--no-phone-home"
    "--ptformat"
  ] ++ lib.optional optimize "--optimize";

  env = {
    NIX_CFLAGS_COMPILE = toString [
      "-D_GNU_SOURCE"
      "-I${lib.getDev pkgs.serd}/include/serd-0"
      "-I${lib.getDev pkgs.sratom}/include/sratom-0"
      "-I${lib.getDev pkgs.sord}/include/sord-0"
    ];
    LINKFLAGS = "-lpthread";
  };

  postInstall = ''
    install -vDm644 "build/gtk2_ardour/ardour.xml" \
      -t "$out/share/mime/packages" || true
    install -vDm644 "build/gtk2_ardour/ardour9.desktop" \
      -t "$out/share/applications" || true
    for size in 16 22 32 48 256 512; do
      install -vDm644 "gtk2_ardour/resources/Ardour-icon_''${size}px.png" \
        "$out/share/icons/hicolor/''${size}x''${size}/apps/ardour9.png" || true
    done
    install -vDm644 ardour.1* -t "$out/share/man/man1" || true
  '' + lib.optionalString videoSupport ''
    wrapProgram "$out/bin/ardour9" \
      --prefix PATH : "${lib.makeBinPath (with pkgs; [ harvid xjadeo ])}"
  '';

  meta = with lib; {
    description = "Ardour DAW – pyrotek45 test/all-fixes patched build";
    homepage    = "https://github.com/pyrotek45/ardour";
    license     = licenses.gpl2Plus;
    mainProgram = "ardour9";
    platforms   = [ "x86_64-linux" ];
  };
}
