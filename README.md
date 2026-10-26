# Syphon Filter: The Omega Strain — native port (play-test build)

An unofficial native port of the PlayStation 2 game *Syphon Filter: The Omega Strain* (2004) to **Windows** and
**PS Vita**. It is **not an emulator**: the game's systems (mission scripts, AI, animation, collision, combat, menus,
level loading, sound) are rebuilt in C++ from the original USA executable and checked against it.

**No game data is included.** You need your own 1:1 image of the USA disc (SCUS-97264); the port reads it at run
time and never modifies it. The required size and SHA-256 are in [docs/REQUIRED_DISC.md](docs/REQUIRED_DISC.md).

This repository is the play-test snapshot of the port's source, updated from the development repository as work
lands. Each update's commit message names the development commit it was taken from.

## What works

| Area | State |
| --- | --- |
| Front end | Boot movies, title, options, agent profiles and memory card, command centre, loadout, loading screens, debriefs |
| Levels that play to their end | **17 of 18:** TRAINING, TORONTO1, TORONTO2, TORONTO3, BELARUS1, BELARUS2, UKRAINE, MNTNEGR1, TOKYO, YEMEN1, YEMEN2, ITALY, KYRGSTAN, MINSK, ZURICH, LORELEI, CHECHNYA |
| Widescreen | True 16:9 (and wider) in levels and menus: a wider view with the HUD, headers and button hints anchored to the screen edges. 4:3, pillarbox and stretch options |
| Controls | Gamepads and keyboard / mouse |
| Port Settings page | Display, control and port options inside the game's own Options menu |
| CPU co-op partners (port-only) | 1–3 CPU agents that follow, fight and climb, and take orders from a command menu (D-pad Up). They do the co-op objectives in TORONTO1–3, BELARUS2 and UKRAINE |
| Mods (port-only) | Replace any disc file, texture (PNG / DDS, any resolution), character, gun or level model (glTF 2.0) without touching your disc ([docs/MODDING.md](docs/MODDING.md)) |
| LAN Play (port-only) | **Play LAN** on the Offline / Online screen: host, browse and join on the local network (peer to peer, no server). Players and enemies are synced in the level |
| Online (port-only server) | `omega_server`: accounts, lobby, game list, create / join, chat, contacts, stats, admin tools, relay and UPnP ([docs/ONLINE_DEPLOY.md](docs/ONLINE_DEPLOY.md)) |
| PS Vita | Boot, intro movies and all menus run on real hardware in 16:9. The first in-level build (TORONTO1 / TRAINING) is being tested |

## What does not work yet

- **Level still in progress:** MYANMAR loads and plays part of the way, then
  stop. When the port reaches something that is not ported yet, it stops with an "unhosted …" or "… not ported"
  message naming the original function; it never guesses. Please report where it stopped (see below).
- **CPU partners:** they don't use lifts yet, BELARUS1's co-op objective "save all 7 workers" is not escorted, and
  stealth behaviour (silenced weapons, disguises, stopping alarms) is planned. Choose the number of partners with
  `coop_partners=1|2|3` in `local\port_settings.txt`.
- **LAN in-level sync:** damage to enemies the other console controls, their aim and fire, and shared objectives are
  still being finished. Expect desyncs.
- **Online:** the server works, but in-level online play has the same gaps as LAN. Agency cells (clans) are not done.
- **PS Vita:** in-level play is brand new and still being tested on hardware. Speed (30 fps target) and memory are
  being worked on.

## Playing on Windows

You need 64-bit Windows 10 or 11, a Direct3D 11 GPU and your disc image.

1. Build the game (below), then make a play folder:
   ```powershell
   .\tools\package_release.ps1 -Out C:\Games\OmegaStrain
   ```
2. Start `C:\Games\OmegaStrain\OmegaStrain.exe`. It finds your disc image (or asks for it), verifies it once,
   derives a few tables from it and starts the game.

Controls, settings (`local\port_settings.txt`), cheats, CPU partners, mods, saves and online hosting are in the player
guide, [tools/package/README-PLAY.md](tools/package/README-PLAY.md), which is copied into the play folder.

## Playing on PS Vita

You need a Vita (or PS TV) with HENkaku / Enso, VitaShell, and `libshacccg.suprx` in `ur0:data/`. That file is the
shader compiler: extract it from your own firmware, for example with the ShaRKBR33D homebrew (other vitaGL homebrew
needs it too).

1. Build `omega_vita.vpk` (below) and install it with VitaShell.
2. Copy your disc image to `ux0:data/omega_strain/omega_strain.iso`.
3. Start **Omega Strain**. The first start compiles the shaders (with a progress bar) and prepares data from the
   disc, which can take a minute or more on black screens. Later starts are quicker.

**Controls:** the buttons are as on the PS2, and L / R are L1 / R1. The touch panels stand in for L2 / R2 / L3 / R3:
on each panel, top-left is L2, top-right R2, bottom-left L3 and bottom-right R3. Either panel can be turned off in
`ux0:data/omega_strain/vita_input.ini`. START + SELECT quits, and SELECT with R held shows the frame-rate overlay.
Settings use the PC's format in `ux0:data/omega_strain/port_settings.txt` (for example `aspect=pillarbox`).

## Building

### Windows

Needs Visual Studio 2022 or later with **Desktop development with C++** (it includes MSVC x64, CMake and Ninja).

```powershell
.\tools\build_native.ps1 -Configuration Release
```

The game is `local\build-native\Release\OmegaStrain.exe`. The build also makes:
- `omega_play.exe`: the same game with diagnostic options;
- `omega_mod_tool.exe`: exports textures and models from your disc for modding;
- `omega_server.exe`: the online server. It also builds on Linux; see [docs/ONLINE_DEPLOY.md](docs/ONLINE_DEPLOY.md).

### PS Vita

Needs, on Windows:
- [VitaSDK](https://vitasdk.org/), Windows-native (default `C:\vitasdk`, or set `VITASDK`), with **vitaGL**,
  **mathneon** and **vitashark** installed into it (for example with `vdpm`);
- Python 3, which draws the LiveArea images during the build;
- CMake and Ninja (the Visual Studio ones are found automatically, or put them on `PATH`).

```powershell
.\tools\build_vita.ps1 -Jobs 4
```

The package is `local\build-vita\omega_vita.vpk`. Add `-Reconfigure` after a toolchain change.

The native GXM renderer is a second package (title SFOS00004, installs beside the first): configure
`local\build-vita-gxm` with `-DOMEGA_VITA_RENDERER=gxm -DOMEGA_VITA_TITLE_ID=SFOS00004 -DBUILD_TESTING=OFF` and the
VitaSDK toolchain file, then build its `omega_vita.vpk` target.

### PS5 (homebrew)

Needs Linux or WSL (Ubuntu 24.04) with clang 18 / LLVM 18, CMake, Ninja, make, Python 3 and rsync, plus the PS5
homebrew toolchain in `~/ps5`: the public [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) v0.42, the
ps5-native-app-boilerplate (revision 4f531c4) and the ps5-opengl SDK 1.0.1 (`tools/ps5/setup_ps5_toolchain.sh`
unpacks them; the last two are not publicly downloadable at the moment). Then, from Windows:

```powershell
.\tools\build_ps5.ps1
```

or on Linux `cmake -S . -B local/build-ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ps5/ps5.toolchain.cmake
-DCMAKE_BUILD_TYPE=Release` and `cmake --build local/build-ps5 --target omega_ps5_package`. The folder title is
`PPSA99720` (`local\build-ps5\PPSA99720`, or `local/build-ps5/app/dist/PPSA99720`).

### Automatic builds

Every push builds the Windows package and both Vita packages on GitHub Actions (`.github/workflows/build.yml`; the
files are under each run's **Artifacts**). The PS5 job runs only on a self-hosted runner with the PS5 toolchain
(the repository variable `PS5_RUNNER` names its label).

## Reporting problems

Open an issue saying what you did and what happened, with the log:
- **Windows:** `local\omega_strain.log` in the play folder, plus both files in `local\crashes\` if it crashed or
  stopped with an error;
- **Vita:** `ux0:data/omega_strain/omega_vita.log`, plus the newest `ux0:data/psp2core-*.psp2dmp` after a crash.

If a level stopped, say which level and roughly where. Never attach game files or disc data.

## Legal

This repository contains only the port's own code and documents: no game files, extracted assets, disc image or
executable, and none may be added. *Syphon Filter* is a trademark of Sony Interactive Entertainment. This is an
unofficial fan project, not affiliated with or endorsed by Sony or Bend Studio.
