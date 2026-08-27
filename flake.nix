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

              # Compress/extract shells out to these by name. Without them
              # the archive menu entries are present but every one of them
              # fails, so they are not optional the way the tools below are.
              pkgs.gnutar
              pkgs.gzip
              pkgs.bzip2
              pkgs.xz
              pkgs.zstd
              pkgs.zip
              pkgs.unzip
              pkgs.p7zip # 7z
              pkgs.libarchive # bsdtar, the fallback for rar and odd formats

              # Optional, and each degrades gracefully on its own (search
              # falls back to a directory walk, previews to plain text) while
              # the startup dependency check names whatever is missing. These
              # three are cheap and cover features a user would notice losing.
              # Deliberately not here: git (+385MB) and exiftool (+127MB) --
              # this is a --prefix, so a user who has them keeps using theirs,
              # and the git-status column and metadata panel simply stay empty
              # for anyone who does not. ffmpeg is out for the same reason.
              pkgs.fd
              pkgs.bat
              pkgs.poppler-utils # pdftoppm, pdfinfo
            ])

            # File and folder icons come from the system icon theme via
            # QIcon::setThemeName() (default "Adwaita", falling back through
            # breeze/Papirus/Adwaita/hicolor). A desktop session normally
            # provides these through XDG_DATA_DIRS, but `nix run` on a minimal
            # or non-NixOS host has neither the variable nor the themes, and
            # every file and folder renders as blank space.
            "--prefix"
            "XDG_DATA_DIRS"
            ":"
            "${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share"

            # gvfs ships the client-side GIO module (libgvfsdbus.so). Without
            # it GIO cannot speak the gvfs protocol at all, so sftp:// smb://
            # and mtp:// transfers fail even when gvfsd is running — hyprfm
            # copies to those URIs in-process via g_file_copy(). NixOS with
            # services.gvfs.enable already exports this, but a plain `nix run`
            # on a non-NixOS host cannot borrow the host module: it is built
            # against the host's glib, not this closure's.
            "--prefix"
            "GIO_EXTRA_MODULES"
            ":"
            "${pkgs.gvfs}/lib/gio/modules:${pkgs.glib-networking}/lib/gio/modules"
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
