{
  description = "HyprFM - a lightweight Qt6/QML file manager for Hyprland";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Mirrors the two git submodules pulled in by PKGBUILD's `git submodule
    # update` (src/qml/icons, src/qml/Quill). Plain flake sources don't fetch
    # submodules, so they're pinned here instead and copied into place in
    # postPatch. Pinned to an exact commit rather than a branch: the commits
    # hyprfm's .gitmodules reference aren't always reachable from quill(-icons)'s
    # default branch, so tracking the branch can silently resolve to an older
    # commit. Keep these revs in sync with `git ls-tree main src/qml/Quill
    # src/qml/icons` whenever the submodules are bumped.
    quill-icons = {
      url = "github:soyeb-jim285/quill-icons/10db5facf6a560e60d2693ccd1909267ef436002";
      flake = false;
    };
    quill = {
      url = "github:soyeb-jim285/quill/bc13deae669a1333a0d7bdd991c7015270a16a38";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      quill-icons,
      quill,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = nixpkgs.lib.genAttrs systems;
      pkgsFor = system: import nixpkgs { inherit system; };

      # Single source of truth for the version lives in CMakeLists.txt
      # (`project(hyprfm VERSION x.y.z ...)`) so it never has to be bumped
      # in two places.
      version = builtins.head (
        builtins.match ".*project\\(hyprfm VERSION ([0-9]+\\.[0-9]+\\.[0-9]+).*" (
          builtins.readFile ./CMakeLists.txt
        )
      );

      mkHyprfm =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "hyprfm";
          inherit version;

          src = self;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            kdePackages.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            kdePackages.qtbase
            kdePackages.qtdeclarative
            kdePackages.qtsvg
            kdePackages.qtwayland
            kdePackages.kwindowsystem
            glib
          ];

          # Submodules aren't fetched for a plain flake source; drop the
          # pinned inputs in where CMake/QML expect them instead.
          postPatch = ''
            rm -rf src/qml/icons src/qml/Quill
            cp -r --no-preserve=mode,ownership ${quill-icons} src/qml/icons
            cp -r --no-preserve=mode,ownership ${quill} src/qml/Quill
          '';

          cmakeFlags = [
            "-DBUILD_TESTS=OFF"
            "-DHYPRFM_DATA_DIR=${placeholder "out"}/share/hyprfm"
          ];

          qtWrapperArgs = [
            "--prefix"
            "PATH"
            ":"
            (pkgs.lib.makeBinPath [
              pkgs.rsync
              pkgs.glib # gio
              pkgs.xdg-utils
              pkgs.wl-clipboard
            ])
          ];

          meta = with pkgs.lib; {
            description = "A lightweight Qt6/QML file manager for Hyprland";
            homepage = "https://github.com/soyeb-jim285/hyprfm";
            license = licenses.mit;
            mainProgram = "hyprfm";
            platforms = systems;
          };
        };
    in
    {
      packages = forEachSystem (system: {
        default = self.packages.${system}.hyprfm;
        hyprfm = mkHyprfm (pkgsFor system);
      });

      apps = forEachSystem (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.hyprfm}/bin/hyprfm";
          meta.description = "A lightweight Qt6/QML file manager for Hyprland";
        };
      });

      devShells = forEachSystem (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.hyprfm ];
          };
        }
      );
    };
}
