# Installing on NixOS

Lichii ships a [`flake.nix`](../flake.nix) that builds the emulator from source
against nixpkgs' Qt and FFmpeg, so it does not carry the AppImage's bundled Qt
6.8 or Ubuntu FFmpeg sonames.

The package installs:

| Path | Contents |
|---|---|
| `bin/lichii` | Qt frontend (wrapped with `qt6.wrapQtAppsHook`) |
| `bin/eden-cli`, `bin/eden-room` | command line and room frontends |
| `bin/nx2_switches.txt` | NX2 switch defaults, read next to the executable |
| `share/applications/io.github.zortos293.lichii.desktop` | desktop entry |
| `share/icons/hicolor/256x256/apps/io.github.zortos293.lichii.png` | icon |
| `share/metainfo/io.github.zortos293.lichii.metainfo.xml` | AppStream metadata |

Flakes must be enabled (`nix.settings.experimental-features = [ "nix-command" "flakes" ];`).

## Quick install into the user profile

```sh
nix profile install github:Frizti/Fritchii#lichii
# or from a checkout:
nix profile install /path/to/Fritchii#lichii
```

`~/.nix-profile/share` is in `XDG_DATA_DIRS` on NixOS, so the desktop entry and
icon are picked up by the application menu, wofi/fuzzel, and Waybar launchers
without further configuration. `~/.nix-profile/bin` is on `PATH`, so `Exec=lichii`
resolves.

- Launch: pick **Lichii** in the menu, or run `gtk-launch io.github.zortos293.lichii`
  / `lichii` from a terminal.
- If the menu does not show it yet, restart the launcher, or run
  `update-desktop-database ~/.nix-profile/share/applications`.
- Remove: `nix profile remove lichii`.
- Update: `nix profile upgrade lichii`.

This uses the nixpkgs revision pinned in `flake.lock`; only Lichii itself is
compiled locally, everything else is substituted from the binary cache.

## Declarative install (NixOS flake)

Add the flake as an input. `inputs.nixpkgs.follows = "nixpkgs"` is optional: it
avoids a second nixpkgs and reuses the system's Qt/FFmpeg, at the cost of the
`cpmCache` hash having to match that revision (see [Maintainer notes](#maintainer-notes)).

```nix
{
  inputs.lichii = {
    url = "github:Frizti/Fritchii";
    inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, lichii, ... }@inputs: {
    nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        {
          environment.systemPackages = [ lichii.packages.${pkgs.system}.default ];
        }
      ];
    };
  };
}
```

With home-manager, use `home.packages` instead; the desktop entry is then installed
into the home profile.

If the configuration passes packages around through `_module.args` (a common
pattern for local flakes), the equivalent is:

```nix
{
  _module.args.lichiiPackage = lichii.packages.x86_64-linux.default;
}
```

and in any module:

```nix
environment.systemPackages = [ lichiiPackage ];
```

Then rebuild:

```sh
sudo nixos-rebuild switch --flake /etc/nixos#hostname
```

### Waybar

The desktop entry uses `Categories=Game;Emulator;Qt;` and `StartupWMClass=lichii`,
so it works like any other application. A Waybar button can launch it directly:

```jsonc
{ "custom/lichii": {
    "format": "🎮",
    "tooltip": "Lichii",
    "on-click": "gtk-launch io.github.zortos293.lichii",
} }
```

### Updating

```sh
nix flake update lichii   # in the system flake
sudo nixos-rebuild switch --flake /etc/nixos#hostname
```

## Building from a checkout

```sh
nix develop
cmake -S . -B build -G Ninja -DENABLE_WERROR=OFF
cmake --build build --parallel $(nproc)
./build/bin/lichii
```

`nix develop` provides the compilers, CMake/Ninja and every dependency. CMake
downloads the remaining dependencies through CPM, which is allowed inside the
dev shell.

To build the packaged artifact instead:

```sh
nix build .#lichii
nix run .#lichii
```

## Flake outputs

| Output | Purpose |
|---|---|
| `packages.<system>.default`, `packages.<system>.lichii` | the packaged emulator |
| `packages.<system>.cpmCache` | CPM dependency sources, fetched once |
| `devShells.<system>.default` | development shell |
| `formatter.<system>` | `nixfmt-rfc-style` |

## Maintainer notes

### The CPM source cache

`CMakeLists.txt` makes CPM download dependencies at configure time, and Nix builds
have no network. The flake therefore fetches them once in a fixed-output
derivation (`packages.<system>.cpmCache`, which only runs the configure step with
`-DCPM_SOURCE_CACHE=$out`) and reuses that directory as a read-only CPM source
cache for the real build.

The cache is a pure function of `cpmfile.json` and of which dependencies CMake
finds in nixpkgs: anything found as a system package is not downloaded, anything
else is. When either changes, refresh the hash:

1. Set `outputHash = lib.fakeHash;` in `flake.nix`.
2. Run `nix build .#lichii` and let it fail.
3. Copy the `got:` value from the error into `outputHash`.

`CMakeLists.txt` keeps `CPM_SOURCE_CACHE` as a `CACHE PATH`, so a pre-populated
directory can also be passed with `-DCPM_SOURCE_CACHE=/somewhere` outside the
flake; a normal variable would shadow that cache entry.

### Other caveats

- `-DENABLE_WERROR=OFF` is set in the flake only; gcc 16 and clang 21 warn on
  plenty of existing code. The upstream `-Werror` default is unchanged.
- No udev rules are installed (`YUZU_INSTALL_UDEV_RULES=OFF` default). Set it on
  to install `dist/72-eden-input.rules` into `lib/udev/rules.d` and copy it to
  `/etc/udev/rules.d` for gyro access.
- `aarch64-linux` evaluates but is not built in CI.
- Flake builds only see git-tracked files, so new sources must be `git add`ed
  before `nix build`.
