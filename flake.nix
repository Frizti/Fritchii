{
  description = "Lichii — Nintendo Switch emulator (Eglantine fork)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      version = self.shortRev or self.dirtyShortRev or "unstable";

      # Libraries the emulator is built against. Anything CPM can find here is
      # used from nixpkgs; the rest is fetched into the CPM source cache below.
      libraries =
        pkgs: with pkgs; [
          boost
          fmt
          nlohmann_json
          lz4
          zstd
          zlib
          openssl
          enet
          libopus
          httplib
          cpp-jwt
          ffmpeg-headless
          libusb1
          cubeb
          gamemode
          vulkan-headers
          vulkan-utility-libraries
          vulkan-loader
          spirv-headers
          spirv-tools
          glslang
          sdl3
          qt6.qtbase
          qt6.qtcharts
          qt6.qtmultimedia
          qt6.qtwayland
          qt6.qttools
          qt6.qt5compat
        ];

      tools =
        pkgs: with pkgs; [
          cmake
          ninja
          pkg-config
          python3
          patch
          git
        ];

      commonFlags = cpmCache: [
        "-DENABLE_WERROR=OFF"
        "-DYUZU_TESTS=OFF"
        "-DBUILD_TESTING=OFF"
        "-DUSE_CCACHE=OFF"
        "-DCPM_SOURCE_CACHE=${cpmCache}"
      ];

      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          # CPM downloads dependencies at configure time. Nix builds have no
          # network, so fetch them once in a fixed-output derivation and reuse
          # that directory as CPM's source cache for the real build.
          cpmCache = pkgs.stdenv.mkDerivation {
            pname = "lichii-cpm-sources";
            inherit version;

            src = ./.;

            nativeBuildInputs = (tools pkgs) ++ [ pkgs.unzip ];
            buildInputs = libraries pkgs;

            # CPM uses CMake's file(DOWNLOAD), which needs a CA bundle in the sandbox.
            SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

            # Run `nix build .#lichii` once per CPM set update; on hash mismatch
            # Nix prints the new value for outputHash.
            outputHashMode = "recursive";
            outputHashAlgo = "sha256";
            outputHash = "sha256-ZdPWPP0ekwczhVgrIrW9MaFI8KGen4lbIqnWQhOyUwM=";

            dontBuild = true;
            dontInstall = true;
            dontFixup = true;
            dontWrapQtApps = true;

            configurePhase = ''
              runHook preConfigure
              mkdir -p "$out"
              cmake -S . -B build -G Ninja \
                -DENABLE_WERROR=OFF -DYUZU_TESTS=OFF -DBUILD_TESTING=OFF -DUSE_CCACHE=OFF \
                "-DCPM_SOURCE_CACHE=$out"
              runHook postConfigure
            '';

            buildPhase = "true";
          };

          lichii = pkgs.stdenv.mkDerivation {
            pname = "lichii";
            inherit version;

            src = ./.;

            nativeBuildInputs = (tools pkgs) ++ [
              pkgs.unzip
              pkgs.qt6.wrapQtAppsHook
            ];
            buildInputs = libraries pkgs;

            cmakeFlags = commonFlags "${cpmCache}";

            # Copied next to the executable at build time; installed by hand.
            postInstall = ''
              install -Dm644 ${./dist/nx2_switches.txt} "$out/bin/nx2_switches.txt"
            '';

            meta = {
              description = "Nintendo Switch emulator (Lichii, a fork of Eglantine)";
              homepage = "https://github.com/Djihads80/Eglantine";
              license = lib.licenses.gpl3Plus;
              platforms = systems;
              mainProgram = "lichii";
            };
          };
        in
        {
          inherit lichii cpmCache;
          default = lichii;
        }
      );

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages =
            (tools pkgs)
            ++ (libraries pkgs)
            ++ (with pkgs; [
              clang
              lld
              mold
              ccache
              jq
              unzip
              mesa
              vulkan-tools
            ]);

          shellHook = ''
            # Off NixOS a host Vulkan driver may not resolve inside the shell;
            # fall back to Nix's Mesa if nothing else is set up.
            if [ -z "''${VK_DRIVER_FILES:-}" ] && [ ! -d /run/opengl-driver ]; then
              export VK_DRIVER_FILES="${pkgs.mesa}/share/vulkan/icd.d/radeon_icd.${pkgs.stdenv.hostPlatform.parsed.cpu.name}.json"
            fi

            echo "Lichii dev shell"
            echo "  cmake -S . -B build -G Ninja -DENABLE_WERROR=OFF"
            echo "  cmake --build build --parallel \$(nproc)"
            echo "  ./build/bin/lichii"
          '';
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
