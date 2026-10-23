# Mods: the asset override layer (port-only)

Checkpoint 132md1. **Not game behaviour:** the PS2 game reads only its disc. This is an optional layer of the PC
port that lets a mod replace disc files, HOG archive members and textures. Anything a mod does not supply comes from
the user's own disc image, so every mod is an override with a fallback to the original asset. With no `mods` folder,
no enabled mod, the setting `mods=0` or the flag `--no-mods`, nothing is installed and the port is bit-identical to
the game without this layer. The checks behind that claim are listed under "Evidence".

Scope:

- **Stage 1 (built):** the override layer: any disc file or archive member replaced by a file of the same format,
  with per-mod manifests and priorities, deterministic resolution, the disc as the fallback, a log of every
  override and a setting / flag to turn mods off.
- **Stage 2 (built):** texture replacement (PNG / DDS at any resolution) and the mod tool that exports a starting
  point (textures as PNG, or raw members) from the user's disc; tests and evidence for both stages.
- **Stage 3 (character models built, checkpoint 132md2):** glTF 2.0 export of a character's original skeleton and
  reference mesh, and replacement parts rigged to that skeleton (validated; the original drawn on a mismatch), drawn
  through the port's skinning at any vertex count with full-resolution textures. See "Stage 3: character models".
- **Stage 3b (level static models built, checkpoint 132md2):** glTF 2.0 export of a level's models (terrain pieces,
  the sky, objects, NI models) and replacement of their drawn mesh at any vertex count with full-resolution images,
  keeping the original collision. See "Stage 3b: level static models".
- **Sounds, music and interface art (built, checkpoint 132md4):** voice and music streams replaced by WAV or Ogg
  Vorbis files, fitted to the game's stream path; interface art, fonts and loading screens replaced as textures.
  See "Sounds and music" and "Interface art and fonts".
- **Online (built, checkpoint 132md4):** the port reports its mods (a class and a fingerprint) to omega_server,
  which can refuse games from gameplay-modded or any modded clients. See "Online: the mod report".
- **Higher resolution and the author guide (checkpoint 132md6):** textures past the disc's sizes (4× level,
  character, interface and font images) and character / gun parts past the originals' vertex and palette limits, with
  the limits; one worked example per kind of mod and a synthetic example mod. See "Higher resolution" and "Mod
  author guide".
- **Out of scope:** the FMV movies (`ZMOVIES.HOG` PSS streams) and the executable.

## Where the port reads game assets

All game data comes from the user's ISO through one class, `DiscImage` (`src/port/disc/disc.hpp`):

- `file(components)` returns a file's extent;
- `read(extent)` reads it;
- `archive_member(archive, member)` / `archive_member_names` read HOG members;
- `hog_member` (the HOG index, research/FINDINGS.md) locates a member inside an archive image.

Every host (the play, the front end, the loading screen, the tools) opens files through it. That makes it the single
boundary the override layer hooks. The main asset kinds and their disc paths:

| Asset | Where on the disc | Port reader |
|---|---|---|
| Textures (TDX: paletted / direct GS images, animation frames, mips) | members of `TEX.HOG`, `MAPTEX.HOG`, `NPCTEX.HOG`, `OBJECTS.HOG` (per mission `GAMEDATA/<MISSION>/`), `COMMON/MANTEX.HOG`, `MORPHTEX`, `PAR.HOG`, `SPECFX.HOG`, gun HOGs in `WPNHOG.HOG` / `WPN.HOG`, `FONTS.HOG`, `LOADING.HOG`, `ERROR.TDX`, `FRONTEND/NTSC.HOG` | `decode_tdx` (`src/main/texman/texture.cpp`); `tdx_in_place` for members read in place from an in-memory HOG |
| Character appearance manifests (text) | mission `SKL.HOG` (`PC_MALE.SKL`, NPC manifests) | `character_appearance` (CHARACTER_APPEARANCE.md) |
| Skeletons | `COMMON/SKEL.HOG` (`MALE.SKEL`, `FEMALE.SKEL`; bones by index, no names) | `decode_skeleton`, `skeleton_bind_pose` |
| Character part meshes (skinned VU1 packets, up to 21 palette bones per part, LOD 0/1) | `COMMON/MANSKM.HOG`, `COMMON/MORPH.HOG`, mission `NPCSKM.HOG` | `decode_skinned_mesh` (`src/main/vu1model/skinned_mesh.hpp`, SKM_VERTEX_CONTRACT.md) |
| Face / hand shapes (vertex deltas, not bones) | `COMMON/MORPH.HOG` (`.SKF`, `MALE.BIN` / `FEMALE.BIN`) | MORPH_FORMAT.md |
| Animation | `COMMON/SKA.HOG`, `SKAS.HOG`, mission `SKALEVEL.HOG` | ANIMATION_CLIPS.md |
| Level geometry and collision | mission `DATA.HOG` (one HOG per terrain piece or sky layer, holding its `.VUM` VUMS geometry and `.COL` collision), objects and NI models in `OBJECTS.HOG` (one HOG per model) | FINDINGS.md "Native VUMS geometry and COL5", LEVEL_OBJECTS.md |
| Guns | `COMMON/WPNHOG.HOG` (one HOG per gun model), mission `WPN.HOG`, `COMMON/WEAPONS.WDB` | COMBAT.md (weapon model load `00147150`) |
| GUI and fonts | `COMMON/FONTS.HOG` (`.FNT`), `LOADING.HOG`, `FRONTEND/NTSC.HOG`, `ZEUS.HOG`, `STRINGS.DAT` | FRONTEND.md |
| Scripts, populations | mission `SCRIPTS.HOG`, `DATA.HOG` (`DATA.POP`) | SCRIPT_FORMAT.md, POPULATION.md |
| Voice and music streams (VAG: SPU ADPCM 22050 Hz mono; VPK: 32000 Hz stereo) | mission `SNDVAG.HOG`, `ZMEDIA/SNDVAG.HOG`, `FRONTEND/SNDVAG.HOG` (VAG), `ZMEDIA/MUSIC.HOG` (VPK) | `audio::decode_stream`, `VpkProgressive` (`src/port/platform/audio_formats.hpp`), played by `SoundIopHost` |

## The mods folder

```
mods\                       next to OmegaStrain.exe (the setting mods_folder= moves it)
  my_mod\
    manifest.txt
    files\GAMEDATA\COMMON\MANTEX.HOG\_LEGS_PAN_GCAM0.TDX          an archive member, same format
    files\GAMEDATA\TORONTO1\SCRIPTS.HOG\...                      (any disc file or member, nested HOGs too)
    textures\GAMEDATA\COMMON\MANTEX.HOG\_VEST_V02_GREE0.TDX.png   a texture image, any size
    textures\GAMEDATA\TORONTO1\TEX.HOG\FIRE.TDX.3.png            frame 3 of an animated TDX
    textures\by-hash\0123456789abcdef.png                        every texture with that content hash
    models\GAMEDATA\COMMON\MANSKM.HOG\1VEST_V02_V02-0.SKM.gltf     a character part (stage 3), + .bin / .png
    models\GAMEDATA\TORONTO1\DATA.HOG\TORONTO_DOWNTOWN_A20.HOG\TORONTO_DOWNTOWN_A20.VUM.gltf   a level model (stage 3b)
    sounds\ZMEDIA\MUSIC.HOG\THEME_MAIN.VPK.ogg                    a music stream (132md4), .ogg or .wav
    sounds\GAMEDATA\TORONTO1\SNDVAG.HOG\1_2_1.VAG.wav            a voice line
    music\theme.ogg                                              any file an audio_overrides= line names (132md13)
  hd_pack.zip                 a packed mod (132md10): the same layout in one .zip, read in place ("Mod archives")
```

- **One folder (or one `.zip`, "Mod archives") per mod**, with a `manifest.txt` of `key=value` lines (blank lines and
  `#` comments allowed):

  | Key | Meaning |
  |---|---|
  | `name` | required; shown in the log |
  | `version`, `author`, `description` | free text |
  | `priority` | an integer, default 0; higher wins (the load order) |
  | `enabled` | `1` / `0` / `true` / `false`, default 1; the player's `mod.<folder>=` setting overrides it (132md4) |
  | `overrides` | (132md4) optional: the assets the mod declares, comma-separated keys or prefixes (below) |
  | `game_version` | (132md10) optional: the game the mod was made for, `SCUS-97264` (compared ignoring case and anything but letters and digits); another is logged and flagged in the launcher, and the mod is used anyway |
  | `notes.<platform>` | (132md10) optional free text per platform (`notes.pc=`, `notes.vita=`, ...), shown in the launcher's details (this platform's first) and by `omega_mod_tool list` |
  | `platform` | (132md11) optional: the platform the mod's textures were converted for (`vita`, written by `omega_mod_tool pack --platform vita`; lower-cased); shown as "Packed for vita" in the launcher's details and by `list`, never enforced ("Mods on the Vita") |
  | `audio_overrides` | (132md13) optional: `<target>=<file>` items, comma-separated, replacing disc sounds by name: a music or voice stream, a bank sample, or every sample a bank sound plays (`audio_overrides=THEME_MAIN.VPK=music/theme.ogg`); the one key that may repeat, its lines accumulating ("Audio overrides") |
  | `audio_volume` | (132md13) optional: a percent, 0..400, default 100: the gain on every sound of the mod (`sounds\` and `audio_overrides=`); the player's `mod_volume.<folder>=` wins ("Audio overrides") |
  | `wide.<screen>` | (132md12) optional: native wide backgrounds for a screen, comma-separated `.png` / `.dds` paths inside the mod's folder (`wide.loading=wide/loading_16x9.png`), drawn behind the 4:3 screen when the window is wider than 4:3 ("Wide backgrounds") |

  Unknown keys are ignored, so later fields can be added. A malformed line, a repeated key (but `audio_overrides`) or
  a bad value skips the mod with a log line. A folder without a manifest is ignored.
- **`overrides=`** (checkpoint 132md4) lists what the mod may override, as keys in the addressing below:
  `overrides=GAMEDATA/COMMON/MANTEX.HOG,GAMEDATA/TORONTO1/TEX.HOG`. An item covers that key and everything below it
  (`GAMEDATA/COMMON/MANTEX.HOG` covers its members' files, textures and models); an item ending in `*` is a prefix
  (`GAMEDATA/COMMON/MAN*`); `BY-HASH` covers the `textures\by-hash\` images. Items are case-insensitive, `\` or `/`.
  Every entry of the folder outside the list is ignored with a log line (`<mod>: ignored <entry> (not in its
  manifest's overrides=)`), so the declaration is exactly what the mod can change. Without `overrides=` everything
  in the folder is used (the md1–md3 mods). `omega_mod_tool declare <mod folder>` writes the line from what the
  folder holds. **Stems** (132md12): an item is matched on its stem, the key without a leading `textures/`,
  `models/`, `sounds/` or `files/` folder and without a trailing `.png`, `.dds`, `.gltf`, `.glb`, `.wav` or `.ogg`,
  so `textures/GAMEDATA/COMMON/LOADING.HOG/BAR.TDX.png` covers `BAR.TDX.png` and the `BAR.TDX.dds` that
  `pack --platform vita` makes of it (and not `BAR.TDX.2.png`, the next frame). A prefix item (`*`) is matched as
  written. The one loose match: a sound item naming `X.wav` also covers `X.ogg`.
- **Addressing.** Files are addressed by their original disc path, then the archive member name, then any nested
  member. Keys are compared upper case and joined with `/`, e.g. `GAMEDATA/COMMON/MANTEX.HOG/GABE.TDX`, so the case
  of the file names does not matter. A texture image is the member's name plus `.png` or `.dds`. Frame *n* > 0 of
  an animated TDX is `<NAME>.TDX.<n>.png`.
- **Forbidden keys:** `SYSTEM.CNF`, `SCUS_972.64` and `ZMOVIES.HOG` with its members. The executable is never
  replaced; the derived data, the disc fingerprint and the movies always come from the disc.

### Resolution (deterministic)

1. The enabled mods are ordered by `priority` (highest first), then by folder name (ordinal, case-insensitive). A
   mod is enabled by the player's `mod.<folder>=0|1` setting when there is one (132md4), else by its manifest's
   `enabled=`.
2. For each key the first mod in that order that has it wins. Lower mods with the same key are logged as shadowed
   (`conflict: ... from A (priority 10) shadows B (priority 0)`).
3. Anything no mod supplies is the disc's.

**Files.** `DiscImage::file` resolves a replaced file, or an archive with replaced members, to a virtual extent
(offsets from `DiscImage::virtual_base` = 2^40). `read` serves those extents from the resolved bytes, and the
`archive_member*` calls follow them. A replaced member changes the archive (`rebuild_hog`):

- the original index is kept: header word 0, the member order and the names;
- the offset table is rewritten;
- a replaced member that is not the last is zero-padded to the archive's member alignment, so a member may grow or
  shrink.

A `files\` entry naming a member the archive does not have is logged and ignored.

**Textures.** When an archive with `textures\` entries below it is first read, the layer decodes those members'
original images. It then maps each original's content hash (FNV-1a 64 of width, height and the decoded frame's RGBA)
to the replacement. `decode_tdx` calls the layer's hook on every decoded frame (frame 0 and each animation frame).
A frame whose hash is mapped gets `Texture::replacement`. Otherwise, if a mod has `textures\by-hash\<hash>.png`, that
image is used. Path keys therefore win over `by-hash` keys. Two consequences:

- The same original image used elsewhere (another member or archive with identical pixels) gets the same
  replacement.
- When two path keys share an original image, the first registered wins, and the log says so.

### The log

Every line starts with `Mods (port-only):` and is printed once per distinct message:

```
Mods (port-only): skipped md1_disabled: disabled in its manifest
Mods (port-only): mod md1_recolour "Recolour test (132md1)" 1.0 priority 10: 652 file(s)
Mods (port-only): conflict: textures/GAMEDATA/COMMON/MANTEX.HOG/_VEST_V02_GREE0.TDX from md1_recolour (priority 10) shadows md1_fallback (priority 0)
Mods (port-only): 3 mod(s) active from D:\...\mods
Mods (port-only): files/GAMEDATA/COMMON/MANTEX.HOG/_LEGS_PAN_GCAM0.TDX <- md1_files
Mods (port-only): files/GAMEDATA/COMMON/MANTEX.HOG/NOT_A_MEMBER.TDX ignored: GAMEDATA/COMMON/MANTEX.HOG has no member NOT_A_MEMBER.TDX
Mods (port-only): textures/GAMEDATA/COMMON/MANTEX.HOG/_VEST_V02_GREE0.TDX <- md1_recolour (64x64 -> 256x256)
Mods (port-only): textures/GAMEDATA/COMMON/MANTEX.HOG/_BOOT_COM_ZIPP0.TDX from md1_fallback not used (Truncated PNG chunk); the original is drawn
Mods (port-only): textures/GAMEDATA/COMMON/MANTEX.HOG/NOT_ON_DISC.TDX matched no texture (no such member or animation frame)
```

A bad image never stops the game: it is logged and the original is drawn. `omega_mod_tool list` prints every mod
folder and what the enabled ones override, in resolution order, with the shadowed mods; `omega_mod_tool conflicts`
prints only where they meet (see "Managing mods").

The player's choices add (132md4):

```
Mods (port-only): md1_disabled: enabled in the settings (mod.md1_disabled=1) over its manifest's enabled=0
Mods (port-only): skipped md1_recolour: disabled in the settings (mod.md1_recolour=0)
Mods (port-only): md4_layer: ignored textures/GAMEDATA/TORONTO1/TEX.HOG/UNDECLARED.TDX.PNG (not in its manifest's overrides=)
```

### Settings and flags

| Where | What |
|---|---|
| `mods=0` (local\port_settings.txt) | mods off |
| `mods_folder=<folder>` | the mods folder (default `mods` next to the game) |
| `mod.<folder>=0\|1` | (132md4) that mod off / on, over its manifest's `enabled=`; written by the in-game MODS page, the launcher's mod panel and `omega_mod_tool enable / disable` |
| `--mods <folder>` | install that folder in any run, including hidden, scripted and regression runs |
| `--no-mods` | no mods, overriding everything |
| `lan_mod_policy=gameplay\|any\|cosmetic\|none\|equal` | (132md5) whom a LAN game this player hosts lets join by their mods (see "LAN: the mod report at join"); default `gameplay` (132md16: the same gameplay mods; `any` before) |
| `--lan-mod-policy <policy>` | (132md5) the same for one run, over the setting |
| `mods_texture_cap=<MB>` | (132md9) the texture memory cap: a level's replacement images past it are scaled down at load ("Texture memory"); 0 (the PC's default) is no cap, the Vita's default is 64 |
| `--mods-texture-cap <MB>` | (132md9) the same for one run, over the setting (any run, hidden ones included) |
| `mod_volume.<folder>=<percent>` | (132md13) that mod's audio volume, 0..400, over its manifest's `audio_volume=`; written by `omega_mod_tool volume` ("Audio overrides") |
| `--mods-cpu-skinning` | (132md8, diagnostic) every replacement part skinned on the CPU (the 132md7 path) |

Interactive runs (the launcher, `OmegaStrain.exe`) read the settings. Hidden, scripted and front-end-script runs
install mods only with `--mods`, so the regressions and the captures never pick up a user's mods folder. Such a run
applies the settings' `mod.<folder>=` choices (and `mods=0`) only when it names a settings file (`--port-settings`).

### Managing mods

Checkpoint 132md4. Several mods stack: each declares its name, version, author, priority (the load order) and what
it overrides in its manifest, and the resolution above is deterministic for any set of folders.

- **In the game.** The Port Settings page (pause menu, or the front end's options) has a last page, **MODS** (`L1`
  from DISPLAY wraps to it). Its rows:
  - `ALL MODS ON / OFF`: `mods=`;
  - one row per mod folder in load order (resolution order: `#1` wins), by the manifest's name (upper case, cut to
    17 characters), valued `#<place> ON` / `#<place> OFF` (132md14). **Cross** toggles `mod.<folder>=` (written as
    the panel and `omega_mod_tool` write it: an earlier `mod.<folder>=` line of any case is replaced, 132md6);
    **Left** / **Right** move the mod one place up / down the order (132md14: the manifests' `priority=`, as the
    launcher's Move up / Move down and `omega_mod_tool move` do, "Moving a mod" below; the cursor follows the mod).
    `INVALID` for a folder whose manifest does not parse (it cannot be toggled or moved; the log says why);
  - with more than 7 mods, 6 at a time and a `MORE MODS  1-6 OF n` row that pages them.

  The page is port-only and says so (it sits under the `PORT SETTINGS` title, and every change's log line: `Port settings page (port-only): MOD
  quiet UP (quiet priority 0 -> 6; applies at the next level)`). A change is written at once and applies at the next
  level (132md15, "Mod changes at the next level": the front end's hand-off to a level installs the changed set; a
  level that is running is not reloaded, since the loader threads share the mod set), so a row whose state differs
  from the running set shows `(NEXT)`: a mod enabled or disabled since the set was installed, or an enabled mod whose
  place among the enabled ones moved. The page lists the folder the session uses (`--mods`, else `mods_folder=`,
  else `mods`).
- **From the command line** (`omega_mod_tool`, the same settings file, default `local\port_settings.txt`,
  `--settings <file>` for another):
  - `list [--mods F] [--settings S]`: every mod folder, enabled or not, with its name, version, author, priority,
    state (and whether the settings chose it), `overrides=` and what it supplies (`used: 652 textures`); then the
    scan's log and the enabled set's entries in resolution order;
  - `conflicts [--mods F] [--settings S] [--strict]`: where the enabled mods meet:
    - `shadowed`: the same key from two mods (the higher priority's is used);
    - `layered`: one mod's texture or model, or a member, applies over another mod's replaced archive or member
      (both are used: the result depends on both mods);
    - `ignored`: entries outside a manifest's `overrides=`;

    with a summary line. Exit 0, or with `--strict` 1 when anything is shadowed or ignored (for a packager's check);
  - `enable | disable [--mods F] [--settings S] <mod folder>...`: writes `mod.<folder>=1|0` (the folder must exist
    with a valid manifest; case-insensitive);
  - `declare <mod folder>...`: writes the manifest's `overrides=` from what the folder holds (each entry's archive,
    up to its first `.HOG` component, or `BY-HASH`);
  - `move [--mods F] [--settings S] <mod folder> up|down` (132md5): one place in the order (see below), printing each
    manifest it changed and the new order;
  - `pack [--out <file.zip>] <mod folder>...` (132md10): each folder as one `.zip` ("Mod archives").
- **In the launcher** (132md5, `src/port/app/launcher_mod_panel.cpp`). Hold **Shift** while `OmegaStrain.exe`
  starts, or start it with `--mod-panel`: a window lists the mod folders before the disc is looked for (one row per
  folder in resolution order: `ON` / `OFF` / `BAD`, the priority, name, version, author and `[folder]`; `*` marks a
  choice made in the settings). Its buttons:
  - **Enable / Disable** (or double-click a row): `mod.<folder>=` in `local\port_settings.txt`;
  - **Move up / Move down**: the manifests' `priority=` (below);
  - **All mods on / off**: `mods=`;
  - **Refresh**, **Open folder** (creates and opens the mods folder);
  - **Play** goes on to the game; **Close** quits.

  Below the list, the selected mod's details (description, priority, what enabled it, `overrides=`, what it
  supplies; 132md10: its archive, `game_version=` and `notes.<platform>=`, its place in the load order and which mod
  wins each entry it contests), the enabled set's load order and conflicts as `omega_mod_tool conflicts` prints them,
  and the scan's log ("Load order and conflicts in the launcher"). The panel uses
  the same folder as the game (`--mods` among the launcher's arguments, else `mods_folder=`, else `mods`). Every change
  is written at once, logged (`Mod panel (port-only): ...`) and applies when the game starts; nothing on the disc
  side is read. `--mod-panel-script <file>` drives it for tests and captures (lines `select <folder>`, `toggle`, `up`,
  `down`, `all`, `refresh`, `capture <png>`, `play`, `close`; `--hidden` keeps the window unshown).

  The panel and `omega_mod_tool` share `src/port/mods/mod_manager.cpp` (the in-game MODS page writes the same keys
  through the Port Settings page): the settings writes
  (`mod.<folder>=` keys match any case and replace earlier lines, so a folder has one key), the manifest rewrites
  (the first `key=` line is replaced in place; the file's line endings and other lines are kept) and the conflict
  summary.

  **Moving a mod.** The order is the manifests' priorities (higher first, then the folder name), so a move rewrites
  `priority=`. Moving a mod one place swaps it with its neighbour; of the two ways to do that (raise the upper one
  over the lower, cascading upwards through equal priorities, or lower the lower one under the upper, cascading
  downwards) the one that changes fewer manifests is taken, and on a tie the moved mod's own. Only the mods whose
  order changes are rewritten; each manifest is re-read first and the move is refused if its priority changed
  since the list was read. Disabled mods keep their place, so enabling one later puts it where it was shown.

### Mod archives (132md10)

A mod can be shipped as one file: a `.zip` in the mods folder holding what the mod's folder would (`manifest.txt`,
`files\`, `textures\`, `models\`, `sounds\`), either at the archive's root or inside one top folder that holds
`manifest.txt` (what zipping a folder from Explorer gives). The game, the launcher and `omega_mod_tool` read the
members in place (nothing is extracted; `src/port/mods/mod_archive.cpp`): an archive's path acts as a folder, so every
mod read (the scan, the image readers, the glTF loader and its `.bin` / `.png`, the sounds, the online fingerprint)
takes the member's bytes.

- **The mod's name** (its `[folder]`, its `mod.<folder>=` key and its place among equal priorities) is the file's
  stem: `hd_pack.zip` is the mod `hd_pack`. A folder and a `.zip` with the same name: the folder is used and the
  archive is listed as `BAD` ("a mod named hd_pack is already in the mods folder (the folder is used)").
- **Format.** The classic zip format: members stored or deflated (methods 0 and 8, what every zip tool writes by
  default), each read checked against its CRC-32. Refused, with the mod skipped and the reason logged (`skipped
  hd_pack.zip: Mod archive ...: <problem>`): zip64 (archives or members past 4 GB), encryption, multi-part archives,
  other compression methods, absolute names or `..` / `.` / empty path components, and two names equal ignoring case
  (a folder could not hold both). Directory entries are ignored.
- **Same results.** A packed mod resolves exactly as its folder: the same entries, the same log lines and the same
  play (the 4× pack packed: the same trace, the same 667 texture lines; "Evidence (132md10)").
- **Changing an archive.** The launcher's and `omega_mod_tool move`'s priority change rewrites `manifest.txt` inside
  the `.zip` (the other members' stored data is copied unchanged; written to `<name>.zip.tmp` and renamed over the
  archive). Nothing else writes into an archive: `declare` needs a folder.

**`omega_mod_tool pack [--out <file.zip>] <mod folder>...`** writes each folder as `<folder>.zip` beside it (or
`--out`, one folder only). The bytes are deterministic (names sorted, a fixed time stamp, no extra fields); each member
is deflated, or stored when that is not smaller (PNG and Ogg data). It reads the archive back (every member's bytes)
and indexes it as the game does, refusing a result that differs from the folder:

```
local\test_mods\hd_zip\md6_hd.zip: "Higher-than-original resolution test (132md6)" 1.0, 1029 file(s), 80849286 bytes -> 50088514 bytes (832 deflated, 197 stored); read back, 1017 entries as in the folder
  note: no game_version= in the manifest (this game is SCUS-97264)
```

A folder packed beside itself is noted (the folder is used while both are in the mods folder: move one out).
`--platform vita [--max-size <px>]` (132md11) converts the textures for the PlayStation Vita while packing ("Mods on
the Vita"); `--platform pc` (or none) packs the files as they are.
`omega_mod_tool list` shows a packed mod as `name [name.zip]`, with its `game_version=` (flagged when it is not this
game) and its `notes.<platform>=` lines.

### Load order and conflicts in the launcher (132md10)

The load order is the manifests' `priority=` (higher first, then the name): for every entry (a file, a texture, a
model, a sound) the first enabled mod that has it wins, and anything no mod has is the disc's ("Resolution"). The
launcher shows it:

- the list's header counts the mods and the archives: `2 mod(s) (1 .zip), first wins, then the disc`; a packed mod's
  row ends with its file name, `[md6_hd.zip]`;
- the selected mod's details: `(archive md6_hd.zip, read in place)`, `Made for SLUS-1 - NOT this game (SCUS-97264):
  used anyway`, `Notes (pc, this platform): ...` then the other platforms' notes, `Load order: 1 of 2 (the first wins
  each entry, then the disc)`, and its contested entries, which mod wins each:

  ```
  Contested entries: wins 1, loses 0; 1016 supplied by this mod alone
    wins  models/GAMEDATA/COMMON/MANSKM.HOG/1VEST_V02_V02-0.SKM over rt_blender
  ```

  (`loses <entry> to <mod> (used)` for an entry a higher mod wins; up to 200 lines, then a count);
- under it, the enabled set's order, `Load order (the first wins, then the disc): 1. md6_hd, 2. rt_blender`, then the
  conflicts (`shadowed`, `layered`, `ignored`) as `omega_mod_tool conflicts` prints them.

**Move up / Move down** change the order (rewriting `priority=`, inside an archive too); the details show the new
winners at once.

## Mod changes at the next level (132md15)

Port-only. A change to the mods made while the game runs (the in-game MODS page, the launcher's mod panel or
`omega_mod_tool` writing the settings file, or files added, removed or edited in the mods folder) applies at the next
level, with no restart: at each front-end hand-off to a level (the first mission from the front end, and the next
level after a level's end), `play_main` reads the mods again as at the start (`--mods`, else the settings' `mods=`,
`mods_folder=`, `mod.<folder>=`, `mod_volume.<folder>=`, `mods_texture_cap=`; `--mods-texture-cap` still wins),
rescans the folder and, when the set differs from the running one, installs it:

```
Port settings page (port-only): MOD audio_pack OFF (mod.audio_pack=0; applies at the next level), written to ...
Front end: hand-off to the TORONTO2 play setup (the next level, in this process; ...)
Mods (port-only): skipped audio_pack: disabled in the settings (mod.audio_pack=0)
Mods (port-only): mod hd_pack "Example HD pack" 1.0 priority 5: 3 file(s)
Mods (port-only): applied at the level boundary before TORONTO2: 1 mod(s) active from ...\mods (66 ms; the front end's art loaded before stays until a restart)
```

- **Why at the hand-off, and why it is safe.** The mod set is read by the loader threads (the level's file, texture,
  model and sound loads, the loading-screen host) while a level loads and by the draw while it runs; it is never
  swapped under them. At the hand-off the last level has ended and the next one's loading has not begun, so no
  loader thread is running, and that is the only place it changes. Beyond that:
  - **Nothing is freed.** `mods::install` keeps every set it replaces (`retired`): a raw `const ModSet*` or a
    resolved path held anywhere (a texture's replacement record, a cached file slot) stays valid for the run. A set
    is a few kilobytes of tables; the images and models it loaded are owned by what loaded them.
  - **Every cache is keyed by the set's generation** (it rises with each install): the virtual-file slots
    (`disc.cpp`), the replacement-image cache, the model-override cache and the mod fingerprint, so the next level
    resolves every file against the new set, and the old entries are never used again.
  - **What stays until a restart:** art already loaded with the old set (the front end's screens, fonts and figures,
    which live for the run; each texture holds its replacement image) and the movies' choice for a movie already
    open. The next level's files, textures, models, sounds, music and movies all come from the new set.
- **When the set has not changed** nothing happens (no line): `mods::signature` compares the enabled mods in order
  (folder, archive, volume, the manifest's or archive's size and time), every winning entry (kind, key, winning mod,
  path, and for a folder mod the file's size and time), the movies and the wide backgrounds. A shadowed file's edit
  does not count (it is not used); an archive mod's members are covered by the archive's stamp. The rescan costs a
  few tens of milliseconds (66 ms and 23 ms in the evidence runs, with the two examples).
- **Not with a LAN or online session.** Its peers checked the mod set when they joined ("Online: the mod report",
  "LAN: the mod report at join"), so a run that opened one keeps the set until a restart and says so:
  `Mods (port-only): the mods changed; they stay as they are until a restart (this run opened a LAN or online
  session, whose peers checked them)`.
- **Not in the regression runs.** The boundary check runs only in a front-end run that uses mods or a settings file
  (`--mods`, the mods setting, or `--port-settings`); a hidden run with neither (the play regressions, the level-end
  and chain checks) never rescans, so it is unchanged.
- **The MODS page** says `applies at the next level` and marks `(NEXT)` against the installed set.

## Texture semantics

- **Only the GPU upload changes.** The renderers upload `upload_image(texture)` (scene, GUI, particles). The
  replacement's pixels, mips and raw alpha are used; the original `Texture`'s width, height, pixels and alpha stay
  CPU-side. So UV normalisation (the original's size), the alpha-test decisions made on the CPU, the corona
  renderer's texel-centre UVs and every other reader are unchanged. A replacement may have any size (each side up to
  8192); UVs are normalised, so a 4× image just has more detail.
- **Alpha.** The original GS alpha is 0..0x80 (1.0); the decoded display alpha is `min(255, 2·As)`. A replacement's
  PNG alpha is that display alpha: 255 is opaque, and 128 is the original's half.
  - Textures with raw alpha (`Texture::raw_alpha`, texels above 0x80 used by the additive / subtractive blends) get
    raw alpha `(a+1)/2` from the replacement, so a 255 PNG alpha is GS 0x80.
  - Values above the GS 1.0 cannot be expressed in a PNG replacement; such texels saturate at 1.0.
  - The exporter writes the display alpha, so an unedited export round-trips except for those over-range texels.
- **Palettes.** A paletted original (CLUT 4 / 8 bit) is exported as straight RGBA (the palette applied); the
  replacement is a direct-colour image. An animated TDX has one decoded image per frame (`Texture::animation`), and
  each frame is replaced separately.
- **Mips.** The replacement gets a full box-filtered mip chain. The scene samples whatever chain it is given, with
  the host's derivative LOD (`scene_renderer.cpp`, the same rule as the originals' decoded mips). A replacement of
  an unmipped original is therefore mip-mapped in the scene, so a high-resolution image does not shimmer at a
  distance. The GUI and the particles sample level 0 only, as for the originals.
- **Formats.** PNG (8-bit grey, grey-alpha, RGB, RGBA, palette with tRNS, 16-bit with the high byte kept;
  non-interlaced), uncompressed 32-bit DDS (legacy RGBA masks, or DX10 R8G8B8A8 / B8G8R8A8) and (132md11) BC1 / BC3
  DDS (fourcc `DXT1` / `DXT5`, or DX10 BC1_UNORM / BC3_UNORM; "Mods on the Vita"). Other block formats and
  interlaced PNG are refused, with a log line. The decoders are self-contained (no system codec) so later platforms
  share them.
- **Caching.** Each image is decoded once per (file, alpha mode) and shared. A failed image is remembered and logged
  once.

### Known limits

- **Two decodes of the same texture.** Path keys work by the original image's content hash. A texture the game
  builds from different bytes than the member's own decode (for example a TDX read in place from a bigger HOG
  image, or the play's gun images, whose decoding agent 132wp1 is fixing in `play_main.cpp`) can miss its path key.
  `textures_index.csv` (written by the exporter) lists each export's hash. If a path key does not take, `by-hash`
  with the hash of the runtime image works. Printing the runtime images' hashes from the play is not built yet.
- **Shadows.** The character shadow buffer renders the textured character (CHARACTER_SHADOWS.md), so a recoloured
  character texture also changes the shadow buffer's colour channels. This is render-only.
- **Gameplay files.** A `files\` replacement of scripts, populations, animation clips or collision changes gameplay.
  Only textures, models (stage 3) and sounds (132md4) are presentation-only. In LAN co-op every peer needs the same mods; the session
  does not check this yet.
- **Derived data** (`derive_game_data`, the fingerprint, `--verify-iso`) always reads the disc's own bytes
  (`DiscImage::set_original_only`).

## Tools

- **`omega_mod_tool.exe`** (built with the port, shipped in the package; `src/port/app/mod_tool_main.cpp`):
  - `validate [--iso F] [--mods <folder>] [--settings <file>] [--match TEXT] [--strict] [<mod folder>...]` (132md9)
    checks the enabled mods, or only the folders named, against the disc in one pass ("Validating a mod").
  - `export-textures [--iso F] --out <mod folder> [--match TEXT] [--by-hash] <disc path>...` writes each texture
    (nested archives too) as PNG in the mod layout, plus `textures_index.csv` (key, size, frames, hash).
  - `export-files ... <disc path>[/<member>...]` writes the raw bytes into `files\`.
  - `export-model [--iso F] --out <mod folder> [--skeleton MALE.SKEL|FEMALE.SKEL] <what>...` (132md2) writes
    characters as glTF 2.0 (see "Stage 3: character models"). `<what>` is a character's appearance manifest
    (`GAMEDATA/UKRAINE/SKL.HOG/UKRAINE_GABE.SKL`) or a part (`GAMEDATA/COMMON/MANSKM.HOG/<NAME>.SKM`, or the bare
    member name as play's `--part` takes it), optionally `=<its .TDX>` for the texture. A level model's `.VUM`, or a
    `.HOG` holding them (a mission's `DATA.HOG` / `OBJECTS.HOG`, nested HOGs walked), exports its level models
    instead, `--match TEXT` keeping the keys containing TEXT (see "Stage 3b: level static models"). A gun archive
    (`GAMEDATA/COMMON/WPNHOG.HOG`, a mission's `WPN.HOG`) or one gun in it (`.../WPNHOG.HOG/<GUN>.HOG`) exports the
    guns' parts, `--match TEXT` keeping the guns whose names contain TEXT (132md3, see "Stage 3c: gun models").
  - `members <disc path>` lists an archive's members.
  - `list [--mods F] [--settings S]` shows every mod folder and what the enabled ones override; `conflicts`,
    `enable`, `disable` and `declare` (132md4) manage them (see "Managing mods").
  - `report [--mods F] [--settings S]` (132md4) prints the online mod report of the enabled mods (the gameplay
    entries, the class, the counts, the fingerprint) and whether each server `mod_policy` would allow it (see
    "Online: the mod report").
  - `check-models [--iso F] [--mods F] [--skeleton S] [--match TEXT]` (132md3) loads every model the enabled mods
    install as the play session would (a character part against its skeleton, with its morph targets bound to the
    part's shapes; a gun part against its gun; a level model with its colour patch sets and animated materials) and
    prints one line per model, accepted or refused with the reason the play would log. Exit 1 if any is refused.
    See "Editing in Blender".
  - `export-sounds [--iso F] --out <mod folder> [--match TEXT] <disc path>...` (132md4) writes each VAG / VPK
    stream of the archives as a 16-bit WAV at its own rate and channels (`sounds\<key>.wav`), and (132md5) every
    sample of each 989snd bank (`.BNK`) as a mono WAV at its rate (`sounds\<bank key>\<n>.wav`), plus
    `sounds_index.csv` (key, format: VAG / VPK / BNK / BNK-loop, rate, channels, frames, seconds, file, and the
    bank sounds that use the sample). See "Sounds and music" and "Sound effects in the banks".
  - `check-sounds [--iso F] [--mods F] [--settings S] [--match TEXT]` (132md4) decodes every sound the enabled mods
    install, fits it to its stream (or, 132md5, its bank sample) as the play would, and prints one line per sound,
    accepted (the source's rate, channels and length against the original's) or refused with the reason the play
    would log. Exit 1 if any is refused. 132md13: each `audio_overrides=` target is checked too: the disc sounds it
    names (streams and bank samples of the music, SNDVAG and every level's SND HOGs), its file fitted to each of their
    formats, which of them it supplies and which shared samples it changes for other sounds as well.
  - `volume [--mods F] [--settings S] <mod folder> <percent>` (132md13) writes `mod_volume.<folder>=<percent>`
    (0..400; an earlier line for the folder in any case is replaced), the player's audio volume for that mod over its
    manifest's `audio_volume=` ("Audio overrides"). `list` shows each mod's `audio_overrides=` and its volume.
  - `decode-audio --out <folder> <.wav | .ogg>...` (132md4) decodes files with the port's decoders and writes each
    as a 16-bit WAV at its own rate (to check what the port hears).
  - Without `--iso` it uses the `iso=` line of `local\port_settings.txt`, else an image found next to it. A new mod
    folder gets a manifest with `enabled=0`.
- **`tools/mod_export.py`** wraps the tool with presets:

  | Preset | What it exports |
  |---|---|
  | `level <MISSION>` | TEX, MAPTEX, NPCTEX, OBJECTS, WPN |
  | `character [--match NAME]` | COMMON MANTEX / MORPHTEX, FRONTEND MANTEX |
  | `gun` | COMMON WPNHOG |
  | `gui` | fonts, loading screens, interface art |
  | `model <.SKL or part>... [--skeleton S]` | characters as glTF (`export-model`) |
  | `level-models <MISSION> [--match TEXT]` | the mission's level models as glTF: DATA (terrain, sky), OBJECTS, MAPVUM (the HUD map, 132md4) (`export-model`) |
  | `gun-models [MISSION] [--match TEXT]` | gun models as glTF: COMMON WPNHOG, plus the mission's WPN when named (`export-model`) |
  | `sounds [MISSION] [--match TEXT]` | voice and music as WAV: ZMEDIA MUSIC and SNDVAG, FRONTEND SNDVAG, plus the mission's SNDVAG when named (`export-sounds`, 132md4); the bank samples of FRONTEND SND.HOG and the mission's SND.HOG (132md5) |

  It also passes through `textures`, `files`, `members` and `list`.
- **`tools/level_model_sweep.py`** (132md4) exports every mission's level models and loads them back with
  `check-models`, tabulating them per mission (see "Level models in every mission").
- **`tools/blender_round_trip.py`** (132md3) exports models, re-saves each through Blender's glTF importer and
  exporter on the command line, and checks both with `check-models` (see "Editing in Blender").
- **`tools/blender_round_trip.py --subdivide N`** (132md6) subdivides every mesh N times in Blender (each triangle
  becomes 4^N, the skin weights, shape keys and UVs interpolated, the imported normals kept) as an HD edit, and
  checks the result as above (see "The round trip").
- **`tools/make_example_mod.py`** (132md6) writes a tiny example mod, all synthetic (no disc data, so it may be
  shared or committed): a level texture at 4×, an interface image at 4× and a bank sound effect, with a full
  manifest (see "Mod author guide").
- **`tools/make_test_mods.py`** generates the evidence mods from the user's ISO into `local\test_mods\evidence`
  (never committed):
  - `md1_recolour`: Toronto level textures with R/B swapped, the shirt magenta, the vest red at 4×;
  - `md1_fallback`: a lower-priority vest, a corrupt boots PNG, a key not on the disc;
  - `md1_files`: the trousers member replaced by the desert camo TDX, plus a bad member name;
  - `md1_disabled`.

  `--models` (132md2) writes the model test mods into `local\test_mods\models` instead, each made from the tool's
  export of the regression agent (TORONTO1 `PC_MALE.SKL`), a TORONTO1 NPC (`TOR1_QLF_1.SKL`) and two female agent
  parts:
  - `md2_models` (priority 10):
    - the hair subdivided twice (16× the triangles) with its texture tinted bright green at 4×;
    - the vest inflated along its normals, without an image, so the game's texture stays;
    - the shirt scaled 1.15× about its centre;
    - the left arm tattoo hidden (a skeleton-only glTF);
    - the female agent's hair subdivided and green, and her shirt inflated;
    - the NPC's shirt inflated.
  - `md2_bad` (priority 20, each refused and logged, the original drawn): the boots with a joint renamed, the
    trousers with `bone_01`'s bind moved, a belt with one joint fewer, the other belt not JSON.

  `--level` (132md2) writes the level model test mods into `local\test_mods\level`, made from the tool's export of
  TORONTO1's `DATA.HOG` and `OBJECTS.HOG`:
  - `md2_level` (priority 10):
    - the street piece under the regression start (`TORONTO_DOWNTOWN_A103`) hidden: its collision stays, so the
      agent still stands and walks on it;
    - the nearest textured piece (`A123`) subdivided twice (16× the triangles) with its texture tinted green at 4×;
    - the other replaceable pieces within 24 units of the start lit bright red (`COLOR_0`);
    - the two sky models lit green;
    - every replaceable `OBJECTS.HOG` model scaled 1.2× and lit green.
  - `md2_level_bad` (priority 20, each refused and logged, the original drawn): a piece with a skin, one with a line
    primitive, one not JSON, and one with a primitive without `POSITION`.

  Both leave out the 28 models with colour patch sets or animated materials. `--refused` (132md3) writes their test
  mod into `local\test_mods\refused`:
  - `md3_refused` (priority 10): each of the 28 subdivided (4× the triangles, so the patch sets remap to new
    vertices), its `COLOR_0` red and blue scaled by 0.6 (the patch changes scale with it) and every texture tinted
    green at 2×. The animated materials' images are ignored and logged, so their disc textures stay and animate.
    `LGT_REDWARN_GLOW` (an object) is also scaled 2×.

  `--mission M` (132md4) writes the same for mission M into `local\test_mods\refused` (or `--out`), as `md4_<m>`:
  M's models with colour patch sets or animated materials, treated as above, plus its HUD map models (`MAPVUM.HOG`)
  subdivided and lit red.

  `--guns` (132md3) writes the gun model test mods into `local\test_mods\guns`, made from the tool's export of COMMON's
  `WPNHOG.HOG` and TORONTO1's `WPN.HOG`:
  - `md3_guns` (priority 10): every part of every gun (584 parts of 207 guns, the shells, the gear pack and the
    icons included) scaled 1.3× about the grip (bone 0, the origin) with its texture tinted green at 2×;
  - `md3_guns_bad` (priority 20): the flashlight's parts (`--bad-gun`, default `FLASHLIGHT.HOG`) with `bone_02`'s
    bind moved 0.5, each refused and logged, the original drawn.

  `--sounds` (132md4; needs Python's `soundfile` and `numpy`) writes synthetic sounds, no disc data:
  - `local\test_mods\audio\md4_sounds` (priority 10): the first TORONTO1 voice line (`1_2_1.VAG`) as 4 s of 880 Hz
    beeps, 0.25 s on and off (a 48 kHz stereo WAV), the TORONTO1 music (`TORONTO1_AMB3.VPK`) as a 6 s A chord
    (110, 165, 220 Hz; a 44.1 kHz stereo Ogg Vorbis), and the title theme (`THEME_MAIN.VPK`) as a 5 s C major chord
    built up a note a second (261, 330, 392, 523 Hz; a 32 kHz stereo Ogg Vorbis);
  - `local\test_mods\audio_bad\md4_sounds_bad` (priority 20): the title theme as an Ogg with a corrupted page, and
    a `.mp3` (not a supported extension).

  `--gui` (132md4) writes `local\test_mods\gui\md4_gui` from the `gui` preset's export: the font pages yellow at 2×
  (nearest, so the glyph edges stay where they were) and every other interface image, loading screen and the error
  screen tinted cyan at 2× with a grid.

  `--hd` (132md6) writes `local\test_mods\hd\md6_hd` (see "Higher resolution"): TORONTO1's `TEX.HOG`, the fonts,
  the loading screens, the error screen, the front end's art and the shirt's `MANTEX` texture at 4×, and the vest
  and the 92F's four parts subdivided until each has more than 65,535 vertices, their images at 4×.

Nothing exported from the disc may go into the repository: `mods/` and `local/` are ignored by git, and the test
mods are generated at test time.

## Evidence (132md1)

- **Core tests** (`tests/mod_tests.cpp`, 6 groups):
  - the PNG / DDS decoders against synthetic vectors (filters, dynamic Huffman, palette + tRNS, 16-bit, legacy and
    DX10 DDS, refusals), plus the PNG writer round trip;
  - manifest parsing;
  - resolution order (priority, folder-name tie-break, forbidden keys, shadowed mods, disabled / malformed mods);
  - the HOG member list, `rebuild_hog` (padding, alignment, growth of the last member) and `resolve_bytes` (nested
    archives, missing members, non-archives);
  - texture replacement (alpha, raw alpha, mips, the content hash, by-hash, caching, a broken image falling back,
    removal);
  - the `DiscImage` boundary on the user's ISO (skipped without it): virtual extents, member order kept, priority,
    fallback of every unreplaced member, a path-keyed texture, and the disc's own extents with no mods or
    `original_only`.
- **Neutrality:** the 8 play regressions are Identical with no mods. Toronto's restart and level-end chain are
  unchanged. `--mods X --no-mods`, and a folder holding only a disabled mod, give frame and shadow captures
  byte-identical to the baseline.
- **Override and fallback:** a hidden `--textured` TORONTO1 capture with the four test mods shows:
  - the level textures with R/B swapped;
  - Gabe's shirt magenta and vest red, the vest at 4×;
  - the trousers in desert camo through the member replacement;
  - the corrupt boots drawn as the original.

  The gameplay log is identical to the baseline apart from the Mods lines.

## Stage 3: character models

Checkpoint 132md2. A mod replaces a character's skinned part (an `.SKM` member) with a glTF 2.0 model rigged to the
game's skeleton. Only the drawing changes: the skeleton, the animation (clips, blends, IK), the hit volumes
(HUMAN_COLLISION.md), the collision and everything gameplay reads stay the original's. Takedowns, ledge grabs,
aiming and cutscenes therefore play unchanged, and so does the trace of a scripted run (see "Evidence (132md2)").

### Export

`omega_mod_tool export-model` (or `tools/mod_export.py model`) writes, for each part:

```
<mod>\models\GAMEDATA\COMMON\MANSKM.HOG\1VEST_V02_V02-0.SKM.gltf   the part on its skeleton (the file a mod replaces)
<mod>\models\GAMEDATA\COMMON\MANSKM.HOG\1VEST_V02_V02-0.SKM.bin
<mod>\models\GAMEDATA\COMMON\MANSKM.HOG\1VEST_V02_V02-0.SKM.png    its texture (the part's TDX, decoded)
<mod>\reference\PC_MALE.gltf (+ .bin, .png)                       every part on one skeleton (viewing / editing only)
```

- **A character** (an `.SKL` appearance manifest in a mission's `SKL.HOG`) is resolved as the play session
  resolves it (`make_npc_figure`): its skeleton (`MALE.SKEL` / `FEMALE.SKEL`), and for each part the
  preregistered mesh (COMMON `MORPH.HOG`, the mission's `NPCSKM.HOG`) or texture (the mission's `NPCTEX.HOG`) that
  serves the request, else `MANSKM.HOG` / `MANTEX.HOG`. The file's path is therefore exactly the key the play
  session looks up for that character. Examples: Gabe `GAMEDATA/UKRAINE/SKL.HOG/UKRAINE_GABE.SKL` (15 parts) or
  `MNTNEGR1/.../GABE_COMBAT.SKL`; Lian `GAMEDATA/TOKYO/SKL.HOG/LIAN_COMBAT.SKL` (14 parts, FEMALE.SKEL), or
  `MYANMAR/.../BURMA_LIAN.SKL`; the male agent `GAMEDATA/TORONTO1/SKL.HOG/PC_MALE.SKL` (19 parts, the regression
  player's).
- **Parts** are exported one by one: `GAMEDATA/COMMON/MANSKM.HOG/1VEST_V02_V02-0.SKM=GAMEDATA/COMMON/MANTEX.HOG/
  _VEST_V02_GREE0.TDX`, or bare names (looked up in COMMON `MORPH.HOG` then `MANSKM.HOG`, the texture in `MANTEX.HOG`
  then `MORPHTEX.HOG`). Names starting with `2` default to `FEMALE.SKEL`. The female agent has no `.SKL`: her look
  is the parts the front end's agent editor chooses (`tools\run_play_scratch.ps1 -Female` lists the defaults).
  Teresa has no `.SKL` on the disc either; only her vest parts (`1VEST_TER_V98-0`, `2VEST_TER_V98-0`) are in
  `MANSKM.HOG`.
- **The glTF:**
  - Y-up, facing +Z. The game's model space is Z-up facing +Y; the rotation is (x, y, z) → (−x, z, y).
  - An `Armature` node holds the joints `bone_00` … `bone_NN` (the SKEL record index; the SKEL has no bone
    names) in the skeleton's hierarchy, at the bind pose (translations only), with the inverse bind matrices.
  - The part's LOD 0 mesh at the bind pose: positions, normals, UVs, up to 8 influences (`JOINTS_0/1`,
    `WEIGHTS_0/1`, normalised), triangles from the strips. The material is the part's PNG as base colour
    (metallic 0, roughness 1).
  - The Khronos glTF-Validator (2.0.0-dev.3.10) reports 0 errors, 0 warnings, 0 infos and 0 hints on every export
    tested (45 files: Gabe, Lian, the male agent).
- A new mod folder's manifest has `enabled=0`.

### Replacement rules

A mod's `models\<disc path>\<member>.SKM.gltf` (or `.glb`) replaces that part wherever the play session draws it:
the player, NPCs, the co-op partner and a body switch (`apply_body_request`). Keys and priorities resolve as for
`files\` and `textures\`. The model is loaded once per installation, key and skeleton, then validated against
the skeleton of the character that draws it:

- **Skeleton (`validate_skeleton`).** The file's first skin must name exactly the skeleton's bones:
  - **count:** the same number of joints as the SKEL has bones (21 for MALE.SKEL);
  - **names:** every joint is named `bone_NN` for a distinct bone of the skeleton, in any order in the skin (a
    suffix such as `bone_14_head` is allowed, so bones can carry readable names);
  - **hierarchy:** each joint's parent node is its bone's parent's joint (the root joint's parent is not a joint);
  - **bind pose:** each joint's rest position (its node's world translation, the armature's transform included)
    lies within the tolerance of the SKEL's bind position. The tolerance is 0.5% of the skeleton's bind extent and
    at least 1e-3 (0.0171 for MALE.SKEL). Parents are checked first, so the moved bone itself is named. Joint
    rotations (an editor's bone roll) are allowed: vertices are brought back to the bind space through each joint's
    rest transform and its inverse bind matrix.
- **Mesh (`load_replacement`).**
  - Every primitive of every skinned mesh node is merged; strips and fans are converted to triangles.
  - Vertices are brought back to the game's bind space through each joint's rest skinning.
  - Up to 8 influences per vertex are kept (the heaviest), and the weights are renormalised.
  - Missing normals are computed and missing UVs are zero.
  - At most 1,048,576 vertices.
- **Textures.** The first textured material's base colour image (PNG or DDS, decoded as `textures\`
  images are; JPEG is refused) is uploaded at its full resolution as the part's texture, tinted by its base colour
  factor. Buffers and images may be files next to the `.gltf` (not outside its folder), base64 data URIs or GLB
  chunks. Without an
  image the game's own texture (and any `textures\` replacement of it) is drawn.
- **Hiding.** A glTF with the skin but no mesh hides the part.
- **On any mismatch** the file is refused with one log line naming the first difference, and the original part is
  drawn. A bad file never stops the game.

The log lines:

```
Mods (port-only): models/GAMEDATA/COMMON/MANSKM.HOG/1HAIR_A03_HAIR0.SKM <- md2_models (MALE.SKEL, 908 vertices, 1632 triangles, image 128x128)
Mods (port-only): models/GAMEDATA/COMMON/MANSKM.HOG/1TATT1ARM_U02-0.SKM <- md2_models (MALE.SKEL, 0 vertices, 0 triangles, hidden)
Mods (port-only): models/GAMEDATA/COMMON/MANSKM.HOG/1BOOT_COM_B01-0.SKM from md2_bad rejected for MALE.SKEL (joint 5 is named "spine" (bone_00 .. bone_20 expected)); the disc's model is drawn
Mods (port-only): models/GAMEDATA/COMMON/MANSKM.HOG/1LEGS_PAN_L01-0.SKM from md2_bad rejected for MALE.SKEL (bind pose: bone_01 rests 1 from the skeleton's (tolerance 0.0171)); the disc's model is drawn
Mods (port-only): models/GAMEDATA/COMMON/MANSKM.HOG/1BELT1HOL_T01-0.SKM from md2_bad rejected for MALE.SKEL (bone count 20, the skeleton has 21); the disc's model is drawn
Mods (port-only): models/GAMEDATA/COMMON/MANSKM.HOG/1BELT2HOL_T01-0.SKM from md2_bad rejected for MALE.SKEL (glTF: JSON expected '"' at byte 2); the disc's model is drawn
```

### Drawing

`build_actor_mesh` skins every part on the CPU with the character's committed palette (the same animated, placed
bone matrices the original parts use). A replaced part's triangles are skinned from that palette
(`skin_replacement_into`, up to 8 influences) in place of the part's own geometry, before the mesh goes to the
scene renderer. The renderer, the character shadow buffer, the LOD choice (`003870c0`; a replacement stands in for
LOD 1 too unless the LOD 1 mesh has its own replacement) and the draw order are unchanged. The vertex count has no
fixed limit beyond the loader's (the test mod's hair has 16× the original's triangles).

### Face and hand shapes (morph targets)

Checkpoint 132md3. The `MORPH.HOG` parts (head, mouth, eyes, neck, beard, hands) are deformed by shapes before
they are skinned: lip sync, expressions (`EMO_SHOT`, blinks), an NPC's base face (`SHAPE_KHORSH`) and the hand
poses (`CGRIP`, `AGRIP`, `ET1` …) all weight shapes of the part's SKF (MORPH_FORMAT.md; the expression stepping is
`0038b530`). A replacement keeps them through glTF morph targets named by the shape table's keys.

- **Export.** `export-model` writes a `MORPH.HOG` part that has a `<stem>.SKF` with one morph target per shape:
  - `POSITION` (face and hands) and `NORMAL` (face) offsets in glTF space. These are the original's offsets at
    weight 1, so the reference part posed by a shape is the game's part posed by it.
  - The names are in `mesh.extras.targetNames` (the convention Blender reads and writes), and `mesh.weights` are 0.
  - The tables: a face part has its skeleton's face table (`MALE.BIN` 88 shapes, `FEMALE.BIN` 34: `EAT`,
    `EARTH`, …, `EMO_SHOT`, `SHAPE_KHORSH` …). A hand part has its hand table (male `HAND1` 11 / `HAND2` 17, female
    12 / 17).
  - A name that appears twice in a table is written `NAME@index` for its later entries.
  - The tool prints `, N face/hand shapes` per part.
- **Loading.** The targets of every primitive are merged by name; a target may omit `NORMAL`. A file is refused when:
  - it has targets without as many names;
  - a name is empty or appears twice;
  - a target's accessor size differs from its primitive's `POSITION`.

  Offsets are brought back to the game's bind space the way positions are.
- **Binding.** When the part is drawn for a character, each target is bound to its shape by name. A plain name
  binds the first shape with that key; `NAME@index` binds that entry. A replacement is refused (logged; the disc's
  part is drawn) when one of its names is not in the part's table or two of its targets name one shape. Targets
  may be a subset: shapes without a target do not move the replacement.
- **Drawing.** Each frame, the character's committed shape weights (`morph_face_targets` / `morph_hand_targets`,
  the same slots the original part draws) are summed in slot order as weighted offsets, added to the bind
  vertices, and the result is skinned (`skin_morphed_replacement_into`). With no weighted shape bound, the plain
  skinning runs, so a rest face costs nothing extra.
- **No targets.** A replacement of a shape part without targets is drawn rigid, as in 132md2, and the log says so
  once. The targets of a part that has no shapes for that character (not a face or hand binding) are ignored, with
  a log line.
- **Far LOD and the body switch.** The original applies no shapes at the far LOD or to the parts of a body switch
  (`apply_body_request` builds them without bindings); neither does a replacement there.

The log lines:

```
Mods (port-only): models/GAMEDATA/COMMON/MORPH.HOG/1HEAD_PLA_DEFA0.SKM from md3_morph: 88 morph targets on 88 shapes (MALE.SKEL)
Mods (port-only): models/GAMEDATA/COMMON/MORPH.HOG/1HAND1BAR_NONE0.SKM: no morph targets; the part's 11 shapes (speech, expressions, hand poses) do not move it (MALE.SKEL)
Mods (port-only): models/GAMEDATA/COMMON/MORPH.HOG/1EYE-_PLA_DEFA0.SKM from md3_morph_bad rejected for MALE.SKEL (mesh 0 has two morph targets named "EAT"); the disc's model is drawn
Mods (port-only): models/GAMEDATA/COMMON/MORPH.HOG/1BEAR_PLA_DEFA0.SKM from md3_morph_bad rejected for MALE.SKEL (mesh 0 has 88 morph targets without as many names (mesh.extras.targetNames)); the disc's model is drawn
Mods (port-only): models/GAMEDATA/COMMON/MORPH.HOG/1HAND1BAR_NONE0.SKM: shapes drawn (first weighted: CGRIP 1.000)
Mods (port-only): models/GAMEDATA/COMMON/MORPH.HOG/1HEAD_PLA_DEFA0.SKM: shapes drawn (first weighted: SHAPE_KHORSH 1.000)
```

The `shapes drawn` line is printed the first time each character's replaced part is drawn with a weighted shape.
It names the weighted shapes, so a mod author can see which targets the game uses.

Editing: a sculpted head must keep its shape keys as glTF morph targets with their names (Blender: shape keys,
exported with "Shape Keys" on). New vertices need offsets for each shape that should move
them. A subdivided head interpolates them (`tools\make_test_mods.py --morph` does so).

### Front-end figures

Checkpoint 132md3. The agent figure outside the level draws the same part replacements as the play:

- **The front end's agent figure** (`AgentPreviewRenderer`, `src/port/app/agent_preview.cpp`): GuiAgentCreate's
  `0028d870` (NEW AGENT), GuiAgentModify's `00341f00` (the agent editor) and GuiAgentDisplay's `00361e70` (the
  dossier). It reads its parts from `GAMEDATA/FRONTEND/MANSKM.HOG` / `MORPH.HOG` (copies of the COMMON members) and
  its skeleton from `GAMEDATA/FRONTEND/SKEL.HOG`. Each part is looked up:
  1. by its front-end key, `models/GAMEDATA/FRONTEND/MANSKM.HOG/<part>.SKM.gltf` (a menu-only look);
  2. else by the play's key, `models/GAMEDATA/COMMON/MANSKM.HOG/<part>.SKM.gltf` (or `MORPH.HOG`), so one mod
     changes the agent everywhere.

  The replacement is validated against the front-end skeleton (logged as `FRONTEND/MALE.SKEL` /
  `FRONTEND/FEMALE.SKEL`, a separate cache entry from the play's) and skinned with the figure's palette each pose,
  including the `.BON` bone scales of the agent's build. Its image replaces the part's texture. The figure's log
  line lists the replaced parts:

  ```
  Front end: frame 405 agent model: 17 parts, 3863 triangles; replaced by mods: GAMEDATA/COMMON/MANSKM.HOG/1HAIR_A03_HAIR0.SKM
  ```
- **The play's loadout and stats figures** (GuiEquipmentModify's `002c2340` → `002bfa90`, GuiScoresEquip's
  `0035b390`): they use the player's replacements (the play's keys and skeleton, including after a body switch for
  the stats figure). Each frame the figure's parts are skinned, then the replaced ones re-skinned from the same
  palette (`substitute_models`). In 132md2 these figures drew the original mesh with the replacement's image (the
  wrong UVs); now they draw the replacement. The log line gains the count:

  ```
  Loadout screen: figure 002c2340 -> 002bfa90 drawn (17 parts, 1 replaced by mods (132md3, port-only)); held guns ...
  ```

Neither figure applies shapes (the original's figures have no morph bindings), so a replaced face is drawn at rest.
Refusals are the play's (the same log lines, with the front-end skeleton name).

### Limits (character models)

- **Bone lengths are fixed.** The skeleton is the game's, so proportions (body types) can change only within the
  bones: a taller character cannot have longer legs. Aiming, hand placement and the hit volumes depend on the bones.
- **The menus' figures do not move their faces.** The front-end agent figure and the loadout / stats figures draw
  no shapes (as the original), so morph targets are unused there (below, "Front-end figures").
- **Skinning palette.** The original parts use at most 21 palette bones each; a replacement may use any bone of
  the skeleton.
- **Editors.** The exports pass the Khronos validator and reload exactly. A round trip through Blender 5.2 keeps
  every model the game uses (132md3, "Editing in Blender"); other editors are untested.

## Evidence (132md2)

- **Core tests** (`tests/model_tests.cpp`, 5 groups):
  - the JSON / glTF reader and writer (literals, escapes, member order, malformed text), matrices, the basis
    rotation, GLB, an external buffer and one escaping its folder (refused);
  - skeleton validation against a synthetic skeleton (the core tests the brief asks for): the tolerance and its
    floor; the export passes; a `bone_NN_name` suffix passes; a joint fewer, a renamed joint, a duplicated name, an
    out-of-range name, a re-parented bone, a bind move of 3× the tolerance, an unconverted (rotated) armature, two
    skins, no skin and another skeleton are each refused with the expected message; a move of half the tolerance
    passes;
  - replacement loading: a hidden (mesh-less) file, game-space positions, weights renormalised heaviest first,
    computed and authored normals, skinning by the identity and a posed palette, strips (alternating winding,
    degenerate triangles dropped), a zero weight, an unskinned mesh, a joint outside the skin and a bad name refused,
    the base colour image and factor, GLB and `.gltf` files;
  - the override lookup: nothing without mods, a matching part replaced and one rigged to another skeleton refused
    (both log lines), LOD 1 taking LOD 0's replacement, the cache, per-part substitution, uninstalling;
  - the disc round trip (skipped without the ISO): the first 6 rigid MANSKM parts exported, reloaded and skinned by
    a posed palette give the original part's triangles, positions (worst deviation 0) and UVs.
- **Neutrality:** with no mods the 8 play regressions are Identical, and Toronto's restart and level-end chain are
  unchanged. `--mods local\test_mods\models --no-mods` gives the baseline's frame, shadow and trace byte for byte.
- **Replacement:** hidden `--textured` TORONTO1 captures at frame 100 with `local\test_mods\models`:
  - **male agent:** the shirt is larger, the vest inflated in its own (game) texture, and the hair green;
  - **female agent (`-Female`, FEMALE.SKEL):** the hair is green, and the shirt is inflated (her back patch now
    lies inside it).

  The four `md2_bad` parts are refused with the log lines above and drawn as the original. The NPC replacement
  (TORONTO1 `NPCSKM.HOG` `1SHIR_QLF_LONG0`) loads in a mission-script run. Every modded run's trace is identical
  to its baseline: the models change only the picture.

## Stage 3b: level static models

Checkpoint 132md2. A mod replaces the drawn mesh of a level model: a terrain piece or sky layer (the mission's
`DATA.HOG`), an object or an NI model (`OBJECTS.HOG`), or (132md4) a model of the HUD map (`MAPVUM.HOG`). Only the
picture changes. The model's `.COL` collision, its
declared bounds (the scene culling, the NI boxes), its header flags (the draw bucket) and its texture list stay the
disc's, as do its placement and any path it moves on. A scripted run's simulation is therefore unchanged (see
"Evidence (132md2, level models)").

### Export

`omega_mod_tool export-model --out <mod> GAMEDATA/TORONTO1/DATA.HOG GAMEDATA/TORONTO1/OBJECTS.HOG`, or
`python tools/mod_export.py level-models TORONTO1 [--match SUBWAY_A] --out mods\subway`, writes one glTF per model
at its key:

```
<mod>\models\GAMEDATA\TORONTO1\DATA.HOG\TORONTO_DOWNTOWN_A20.HOG\TORONTO_DOWNTOWN_A20.VUM.gltf  (+ .bin, + one PNG per texture)
<mod>\models\GAMEDATA\TORONTO1\OBJECTS.HOG\CAR_POLICE.HOG\CAR_POLICE.VUM.gltf
```

- One line per model: `<key> (V vertices, T triangles, N textures, F found) -> <file>`. A model whose replacement
  keeps something of the disc's gets `note: a replacement keeps the disc's <what>` ("Colour patches and animated
  materials" below), and one that cannot be replaced (no material) gets `note: a replacement of this model is
  refused: <why>`. TORONTO1 has 541 level models (DATA.HOG 376, OBJECTS.HOG 165). 15 have colour patch sets and 13
  have animated materials; none is refused.
- **Space:** as the character models': the game's Z-up becomes glTF Y-up, (x, y, z) → (−x, z, y). Terrain pieces and
  the sky are in world space; objects and NI models are in their model space (the population places them on load,
  as it places the original).
- **One node, one primitive per drawn material**, holding that material's vertices: positions, normals, UVs and the
  baked light as `COLOR_0` RGB at half scale (0.5 is the game's 0x80, "1.0"). The materials are named `material_NN`
  (NN the VUM material index), with the texture as the base colour image (from the model's own HOG, then the
  mission's `TEX.HOG`, or the reverse for `DATA.HOG`; for `MAPVUM.HOG`, the mission's `MAPTEX.HOG` first, as the
  map binds them) and `alphaMode` `MASK` where the material is alpha-tested.
- Models without drawn triangles export as an empty node (no buffer).
- The Khronos glTF-Validator (2.0.0-dev.3.10) reports 0 errors and 0 warnings on all 541 TORONTO1 exports (45 infos:
  `NODE_EMPTY` on the mesh-less models).

### Replacement rules

A mod's `models\<disc path>\<MODEL>.HOG\<MODEL>.VUM.gltf` (or `.glb`) replaces that model wherever the play
session decodes it. Keys and priorities resolve as for `files\`. Each file is loaded once per installation and key.

- **Meshes.** Every mesh node of the default scene is merged, with its node transforms applied (a mirroring transform
  flips the winding back). Triangles, strips and fans are accepted; strips and fans are converted. Missing normals are
  computed, missing UVs are zero and missing colours are 0x80.
- **Materials.** A primitive's material named `material_NN` (an editor's suffix such as `material_03.001` or
  `material_03_x` is allowed) draws with the disc's material NN: its shader, flags and passes. Any other name, or NN
  past the disc's materials, draws with the model's first drawn material that is not animated (else its first drawn).
- **Colours.** `COLOR_0` RGB is doubled back (0.5 → 0x80) and clamped to 0..0xff (0.996 and above give 0xff); the
  vertex alpha is 1 (0x80).
- **Images.** A material's base colour image (PNG or DDS, as `textures\` images; any resolution)
  replaces that material's base texture for this model only, tinted by its base colour factor. Without an image the
  disc's texture (and any `textures\` replacement of it) is drawn. A material whose original shader draws untextured
  stays untextured: pick a textured `material_NN` for an image.
- **Hiding.** A glTF with no mesh hides the model; its collision stays.
- **Bounds.** The culling uses the original's declared bounds. A replacement reaching outside them is accepted and
  logged (`extends X outside the declared bounds`), but it can be culled early where it sticks out.
- **Limits.** At most 1,048,576 vertices and 2,097,152 triangles per model. The mesh is cut into 64-vertex batches
  for the terrain point-light pass.
- **Refused** (one log line naming the reason; the disc's model is drawn):
  - a skin (a level model is static: remove the armature);
  - a primitive other than triangles, strips or fans (points, lines);
  - malformed glTF or accessors, or the size limits;
  - a `material_NN` naming an animated disc material that the model never draws (it would add an animator);
  - a disc model with no material.

The log lines:

```
Mods (port-only): models/GAMEDATA/TORONTO1/DATA.HOG/TORONTO_DOWNTOWN_A123.HOG/TORONTO_DOWNTOWN_A123.VUM <- md2_level (static, 4550 vertices, 6592 triangles, 9 materials, 9 images); the collision stays the disc's
Mods (port-only): models/GAMEDATA/TORONTO1/DATA.HOG/TORONTO_DOWNTOWN_A103.HOG/TORONTO_DOWNTOWN_A103.VUM <- md2_level (static, 0 vertices, 0 triangles, 0 materials, hidden); the collision stays the disc's
Mods (port-only): models/GAMEDATA/TORONTO1/OBJECTS.HOG/CAR_POLICE.HOG/CAR_POLICE.VUM <- md2_level (static, 3431 vertices, 2217 triangles, 10 materials, 10 images, extends 1.806 outside the declared bounds); the collision stays the disc's
Mods (port-only): models/GAMEDATA/TORONTO1/DATA.HOG/TORONTO_DOWNTOWN_C46.HOG/TORONTO_DOWNTOWN_C46.VUM from md2_level_bad rejected (the model is skinned (a level model is static: remove the armature)); the disc's model is drawn
Mods (port-only): models/GAMEDATA/TORONTO1/DATA.HOG/TORONTO_DOWNTOWN_A35.HOG/TORONTO_DOWNTOWN_A35.VUM from md2_level_bad rejected (mesh 0 has no POSITION); the disc's model is drawn
Mod model images (port-only, 132md2): 492 materials of 310 replaced level model instance(s) drawn with their glTF images
```

### Colour patches and animated materials

Checkpoint 132md3. Until then the port refused the 28 TORONTO1 models whose lights switch their vertex colours or
whose materials animate. They are now replaced too, and keep the game's behaviour:

- **Colour patch sets** (the PNT lights' baked contribution, `003767e0`; LEVEL_LIGHTS.md section 13). The disc's
  sets are remapped to the replacement's vertices (`remap_color_patches`, `src/port/gfx/static_model.cpp`):
  - Each replacement vertex takes its nearest original vertex (game space; a uniform grid; at equal distances the
    vertex with the same colour and UV, then the lower index).
  - Per set, the change is the signed change the original's off state gives at that vertex (the payload bytes summed
    with wrapping, as the walk applies them).
  - The change is scaled by the ratio of the replacement's colour byte to the original's (kept when the original's
    is 0), so a darker replacement dims by as much. It is then clamped so that the off state stays within 0..255.
  - The sets keep their keys and order, so the 10 PNT lights register the same (model, set) slots
    (`00371c30`), and the zone tracker switches them as for the disc's model.
  - An unchanged re-export gets the original's off state exactly, vertex for vertex.
  - The log adds `N colour patch sets on M vertices`.
- **Animated materials** (UV scroll or texture frames, `00376ad0`). The level makes one animator per surface drawn
  with an animated material, in surface order, and the frame animators of mode 5 draw the game's random numbers. A
  replacement is laid out so that the animators are the original's (count, order, parameters):
  - one surface per original animated surface, in its order, with that material;
  - the first of each material holds all its triangles (every primitive naming it), and the others are empty;
  - then the other surfaces, in glTF order.
  - Primitives naming an animated `material_NN` share one material without an image. Their glTF image is ignored
    and logged (`material_10 is animated: its glTF image is ignored, the disc's texture drawn (retexture it under
    textures\)`), because the animator scrolls or steps the disc's texture. A `textures\` replacement of that
    texture applies, frames included.
  - When every drawn material is animated, an unnamed primitive joins the first one.
  - The log adds `K animated surfaces kept`.

```
Mods (port-only): models/GAMEDATA/TORONTO1/DATA.HOG/TORONTO_DOWNTOWN_A07.HOG/TORONTO_DOWNTOWN_A07.VUM <- md3_refused (static, 5898 vertices, 6704 triangles, 24 materials, 24 images, 10 colour patch sets on 1549 vertices); the collision stays the disc's
Mods (port-only): models/GAMEDATA/TORONTO1/DATA.HOG/TORONTO_DOWNTOWN_A26.HOG/TORONTO_DOWNTOWN_A26.VUM <- md3_refused (static, 5523 vertices, 6124 triangles, 18 materials, 17 images, 1 animated surfaces kept); the collision stays the disc's
Mods (port-only): models/GAMEDATA/TORONTO1/DATA.HOG/TORONTO_DOWNTOWN_A26.HOG/TORONTO_DOWNTOWN_A26.VUM: material_10 is animated: its glTF image is ignored, the disc's texture drawn (retexture it under textures\)
```

### Level models in every mission

Checkpoint 132md4. `python tools/level_model_sweep.py` exports every mission's `DATA.HOG`, `OBJECTS.HOG` and
`MAPVUM.HOG` with `export-model`, enables each export as a mod and loads every model back with `check-models` (the
play session's loader, with the colour patch remap and the animated layout). Per mission it counts the models of each
archive (with colour patch sets / with animated materials; a model can have both) and how many load back. It writes
`local\sweep\sweep.md` and `sweep.json` (disc-derived names: keep them out of the repository) and exits 1 on any
refusal or skip. On the USA disc (12 minutes):

| Mission | DATA.HOG (colour / animated) | OBJECTS.HOG (colour / animated) | MAPVUM.HOG | Accepted | Refused | Skipped |
|---|---|---|---|---|---|---|
| BELARUS1 | 570 (0 / 49) | 98 (0 / 2) | 2 | 670 | 0 | 0 |
| BELARUS2 | 591 (0 / 25) | 250 (0 / 4) | 2 | 843 | 0 | 0 |
| CHECHNYA | 142 (0 / 9) | 6 (0 / 1) | 1 | 149 | 0 | 0 |
| ITALY | 102 (0 / 0) | 34 (0 / 0) | 2 | 138 | 0 | 0 |
| KYRGSTAN | 139 (0 / 0) | 24 (0 / 5) | 2 | 165 | 0 | 0 |
| LORELEI | 322 (0 / 56) | 54 (0 / 2) | 4 | 380 | 0 | 0 |
| MINSK | 301 (0 / 3) | 142 (0 / 0) | 5 | 448 | 0 | 0 |
| MNTNEGR1 | 259 (0 / 72) | 10 (0 / 1) | 2 | 271 | 0 | 0 |
| MYANMAR | 915 (0 / 20) | 129 (0 / 0) | 1 | 1045 | 0 | 0 |
| TOKYO | 350 (0 / 14) | 99 (0 / 5) | 3 | 452 | 0 | 0 |
| TORONTO1 | 376 (15 / 12) | 165 (0 / 1) | 2 | 543 | 0 | 0 |
| TORONTO2 | 352 (32 / 12) | 73 (0 / 0) | 3 | 428 | 0 | 0 |
| TORONTO3 | 123 (0 / 2) | 31 (0 / 0) | 3 | 157 | 0 | 0 |
| TRAINING | 54 (0 / 0) | 8 (0 / 0) | 5 | 67 | 0 | 0 |
| UKRAINE | 327 (0 / 6) | 137 (0 / 6) | 2 | 466 | 0 | 0 |
| YEMEN1 | 202 (0 / 3) | 147 (0 / 0) | 4 | 353 | 0 | 0 |
| YEMEN2 | 102 (0 / 5) | 83 (0 / 6) | 4 | 189 | 0 | 0 |
| ZURICH | 168 (0 / 11) | 102 (0 / 14) | 2 | 272 | 0 | 0 |
| **All** | 5395 (47 / 299) | 1592 (0 / 47) | 49 | 7036 | 0 | 0 |

- **Colour patch sets** exist only in TORONTO1 (15 terrain pieces) and TORONTO2 (32, the sewers; 4 of them also
  animate). **Animated materials** are in 346 models of 16 missions (ITALY and TRAINING have none). Both are
  replaceable since 132md3 ("Colour patches and animated materials"). The sweep found no other kind of model.
- **Refused: none.** The one export-time blocker (a disc model with no material) occurs in no mission, and every
  VUM in the three archives decodes. A model is decoded once per name and its copies share one replacement, so a
  model placed many times gets one remap.
- **The HUD map** (`MAPVUM.HOG`, 1 to 5 models per mission: the map screen's floor plans, `0026e070`) is replaceable
  since 132md4. Its glTF images bind in their own slots after the map's `MAPTEX.HOG` textures. Its declared bounds
  (the map's extent and its cull `0026e970`) stay the disc's. The map has no collision, and its draw is unlit.
- **Not level models** (nothing to sweep here): the characters, faces and hands ("Stage 3"), the guns ("Stage 3c"),
  and the particle effects (`SPECFX.HOG` sprites: replace their textures under `textures\`). A GOB's shatter pieces
  are built from its COL quad and its textures, not from a model, so they also follow `textures\`.

### Drawing

The replacement is swapped in where the play session decodes the VUM (`src/port/app/play_main.cpp`: terrain, objects,
NI models, the sky; 132md4: the HUD map's models in `build_map`), before the model is appended to the scene. From there it follows the original's path:
placement, pathed and lift moves, the scene filing, the terrain point lights, the colour of the level lights and
the shadow receivers (a drop shadow lands on the drawn mesh, so a hidden model receives none). Its images are
decoded once per model and material and bound in place of the material's texture (`--textured`).

### Limits (level models)

- A colour patch set follows the nearest original vertex. On a replacement whose shape departs far from the disc's,
  a lamp's light pool stays where the disc's geometry had it.
- An animated material's image cannot come from the glTF (only from `textures\`, for every model drawing that
  texture). Within such a model the animated surfaces draw first; on the disc they may come later, which matters
  only for translucent surfaces overlapping in the same model.
- The collision is the disc's: a replacement with a different shape is walked, shot and seen through as the original.
- Visibility uses the original's declared bounds (above).
- The particle effects are not covered. (Gun models: "Stage 3c: gun models". The front end's agent figure and the
  loadout figure draw the character model replacements: "Front-end figures".)

## Evidence (132md2, level models)

- **Core tests** (`tests/model_tests.cpp`, 3 groups):
  - replacement loading: node transforms (a mirror flips the winding), `material_NN` names and suffixes, strips and
    fans, the colour scale, hidden models, batches, the overshoot, and each refusal (skin, mode, blockers);
  - export → load exactness (positions, UVs, colours, materials, `MASK`), the mod lookup with its log lines, a
    rejection with the disc's model kept, the cache and uninstalling;
  - the disc round trip (skipped without the ISO): 24 TORONTO1 models from `DATA.HOG` and `OBJECTS.HOG` (15478
    triangles) export and reload exactly; 1 is refused.
- **Neutrality:** with no mods the 8 play regressions are Identical, and Toronto's restart and level-end chain are
  unchanged. `--mods local\test_mods\level --no-mods` gives the baseline's frame, shadow and trace byte for byte.
- **Replacement:** a hidden `--textured` TORONTO1 capture at frame 100 with `local\test_mods\level` shows the
  street wall red, the objects green, and the agent standing on the hidden street piece. The four `md2_level_bad`
  models are refused with the lines above. Its trace equals the baseline's in every simulation field (root, motion,
  camera ...) in all 120 frames; only `shadow_receivers` is one lower (the hidden piece receives no shadow).

## Stage 3c: gun models

Checkpoint 132md3. A mod replaces a gun's parts (the body, the clip, the bolt, the holster, the muzzle flare ...),
the shells, the gear pack and the other models the weapon library loads from gun HOGs. As for the characters, only
the picture changes: the attach points, the muzzle and shell points, the hit tests and the ammunition are the
disc's.

### The gun skeleton

A gun model (`GunModel`, `0038d530` / `0038cd90` on a gun HOG such as `WPNHOG.HOG/92BERETTA.HOG`) draws its parts
with eleven matrices (`gun_slot_matrices`, `003870c0`): matrix 0 is the attach point (the hand, the holster, the
HUD preview's identity) and matrix k (1..10) is its parent's (word `+15c + 32k`) moved by the bone offset
`+150 + 32k`. Matrix 3 also turns for the flare (`+2d90` / `+2d91`) and the stun baton (`+415`). As a skeleton
(`mods::gun_skeleton`): 11 bones `bone_00` .. `bone_10`, `bone_00` the root at the origin (the grip), bone k under
its parent at that offset. Every part chunk is local to one bone's matrix; the export moves it by that bone's bind
translation (`ExportPart::bone_local`), so a part sits in one model space with all the gun's other parts.

### Export

`omega_mod_tool export-model --out <mod> GAMEDATA/COMMON/WPNHOG.HOG [GAMEDATA/TORONTO1/WPN.HOG] [--match BERETTA]`,
or `python tools/mod_export.py gun-models [TORONTO1] [--match BERETTA] --out mods\guns`:

```
<mod>\models\GAMEDATA\COMMON\WPNHOG.HOG\92BERETTA.HOG\_BODY_92B_PIST0.SKM.gltf   (+ .bin, + .png)
<mod>\models\GAMEDATA\TORONTO1\WPN.HOG\00SHELLS.HOG\_SHEL_SHE_9MM-0.SKM.gltf
<mod>\reference\92BERETTA.gltf                                                    (the whole gun, for viewing)
```

- The key is `<gun archive>/<GUN>.HOG/<member>.SKM`: the gun HOG's `.SKM` member that holds the part's LOD record
  (`+2c0` / `+340 + 16p`, word +8). LOD 1 records name the same members as LOD 0 on the disc, so one file serves
  both.
- One line per part: `<key> (part P, LOD L, N chunks, textured[, flare]) -> <file>`. The model is built as the play
  session's library builds it (`gun_model_construct` / `gun_model_load`), its textures found as the library finds
  them (`<name>.TDX` in the gun HOG).
- Each file holds the gun's skeleton (rigged as above, in glTF space like the characters') and the part, skinned
  to its bone, with its texture. COMMON's `WPNHOG.HOG` exports 210 guns (522 parts) with no skips; the Khronos
  validator reports 0 errors on them.

### Replacement rules

A mod's `models\<gun archive>\<GUN>.HOG\<member>.SKM.gltf` (or `.glb`) replaces that part of that gun.

- **Keys.** For a model loaded in a mission, the mission's key (`GAMEDATA/<MISSION>/WPN.HOG/...`) is tried first,
  then COMMON's (`GAMEDATA/COMMON/WPNHOG.HOG/...`). The first key a mod supplies wins. If it is refused, the disc's
  part is drawn (the other key is not tried, as for the characters). A COMMON key therefore also replaces the
  mission's copy of the same gun when the mission's copy has the same bones. The front end's weapon display uses
  COMMON's keys.
- **Validation.** As the characters' (bone count, names, hierarchy, bind pose within 0.5% of the skeleton's extent)
  against `gun_skeleton` of the model it replaces. A gun of the same name with other bones (a mission's variant)
  has its own cache entry. The log names the gun HOG:

  ```
  Mods (port-only): models/GAMEDATA/COMMON/WPNHOG.HOG/92BERETTA.HOG/_BODY_92B_PIST0.SKM <- md3_guns (92BERETTA.HOG, 123 vertices, 190 triangles, image 128x64)
  Mods (port-only): models/GAMEDATA/TORONTO1/WPN.HOG/FLASHLIGHT.HOG/_BODY_AFL_OTHE0.SKM from md3_guns_bad rejected for FLASHLIGHT.HOG (bind pose: bone_02 rests 0.5 from the skeleton's (tolerance 0.0068)); the disc's model is drawn
  ```
- **Content.** As a character part: any vertex count, up to 8 influences on any of the 11 bones, a base colour image
  at any resolution (attached over the part's original texture), a mesh-less glTF hiding the part. Rig to `bone_00`
  for the body: the holster draw fills every matrix with matrix 0, so a part on another bone is drawn at the grip
  there, as the original's are.

### Drawing

`mods::skin_gun_replacement_into` skins the replacement with the draw's own eleven matrices, after each bone's inverse
bind (its bind translation removed), so a vertex at the bind pose lands where the original's vertex local to that
bone's matrix does. It replaces `gun_part_geometry` in each draw that has a replacement:

- in play (`play_main.cpp`): the guns held and carried at the attach points (player and NPCs, both LODs), the
  holsters, the shells, the gear pack and the projectiles (thrown grenades). Flare parts keep the flare's draw
  (unlit, the vertex colour 0x7f, the thermal FLA class);
- the HUD and pickup weapon previews and the loadout / stats figures' held guns (`PlayGuiOverlay`);
- the front end's weapon display (GuiWeaponDisplay, `frontend_disc.cpp`).

The overrides are resolved once per model handle (in play when the overlay images are made; elsewhere on the first
draw) and dropped when the weapon library changes. With no mods the draws are the original's.

### Limits (gun models)

- **Bones are fixed** (as the characters'): the muzzle, shell and flare points are the disc's bones.
- **The LOD 1 records** name the LOD 0 members on the disc, so a gun has one replacement for both LODs.
- **No shapes.** Gun parts have no morph targets.

## Sounds and music

Checkpoint 132md4. A mod replaces a voice line or a music stream with a WAV or Ogg Vorbis file in `sounds\`, keyed
like a `files\` entry: `sounds\<disc path>\<member>.wav` or `.ogg`, e.g.
`sounds\ZMEDIA\MUSIC.HOG\THEME_MAIN.VPK.ogg`. `tools/mod_export.py sounds [MISSION]` writes the originals as WAV to
start from. The entries obey the manifest's `overrides=`, the priorities and the shadowing like every other entry; any
other extension below `sounds\` is ignored with a log line.

**Where it plugs in.** The game starts a stream through the IOP sound host (`SoundIopHost`, the port of the game's
989snd stream calls) by its disc sector. The host maps each stream sector to its member key (the HOG indexes of the
sound archives it opened). When a mod has that key, the host plays a clip decoded from the mod's file in place of
the disc's, through the same mixer: the game's volume, pan, pause, stop and loop commands apply unchanged. With no
`sounds\` entry nothing is looked up and the disc's stream is decoded as before.

**Fitting.** The game's timing follows its streams: a voice line's end releases the script waiting on it, the
subtitle and the lip sync. So the replacement is fitted to the original:

- converted to the original's channels (mono: the channels' mean; stereo from mono: the channel repeated) and
  resampled (linear) to the original's rate (VAG 22050 Hz mono, VPK 32000 Hz stereo);
- a stream the game does not loop is cut, or padded with silence, to exactly the original's length, so its end falls
  on the same frame;
- a stream the game loops (the music: the start call's loop flag) keeps its own length and loops at its own end.

**Formats.** WAV: PCM 8 / 16 / 24 / 32 bit, IEEE float 32 / 64, `WAVE_FORMAT_EXTENSIBLE`, any rate and channel
count. Ogg Vorbis: Vorbis I with floor 1 (what every current encoder writes); floor 0 is refused with a message to
re-encode. Both decoders are the port's own (`src/port/mods/audio_files.cpp`, no third-party code); the Vorbis
decoder is within 1 LSB (16-bit) of libvorbisfile on the test vectors.

**Decoding.** A music stream is minutes long, so it is not decoded at its start: the start reads the headers (for
an Ogg, every page and its CRC, and the length from the granule positions), then the samples are decoded a piece
ahead of the mix (`mods::FittedAudio`, as the disc's VPK music is, `VpkProgressive`). A 4-minute 44.1 kHz stereo Ogg
costs 18.7 ms at its start and at most 2.3 ms per 60 Hz frame after (625 ms decoded whole); the samples are identical
to a whole-file decode.

**Refusals.** A file that does not decode (not a WAV / Ogg, truncated, a CRC mismatch, an unsupported codec) is
refused at the stream start with a log line, and the disc's stream plays. A packet that fails later in a long Ogg is
logged (`decoding stopped at X s`); the rest of that stream is silent.

```
Mods (port-only): sound ZMEDIA/MUSIC.HOG/THEME_MAIN.VPK <- md4_sounds (THEME_MAIN.VPK.ogg): 32000 Hz x2 5.00 s -> 32000 Hz x2 5.00 s (a looping stream: its own length)
Mods (port-only): sound GAMEDATA/TORONTO1/SNDVAG.HOG/1_2_1.VAG <- md4_sounds (1_2_1.VAG.wav): 48000 Hz x2 4.00 s -> 22050 Hz x1 13.48 s (the original's length)
Mods (port-only): sound ZMEDIA/MUSIC.HOG/THEME_MAIN.VPK <- md4_sounds_bad (THEME_MAIN.VPK.ogg) refused (Ogg: page CRC mismatch at byte 8579); the disc's stream is played
```

`omega_mod_tool check-sounds` runs the same checks without the game. It cannot know which streams the game loops,
so it reports the source's length against the original's.

### Limits (sounds)

- **Sound effects** are not streams but SPU ADPCM samples inside the 989snd banks (`ENVIRON.BNK`, `SHELL.BNK`,
  `VOX.BNK`, `WEPS.BNK`): see "Sound effects in the banks" (132md5) for replacing them one at a time.
- **Lip sync and subtitles** are the game's, timed to the original line (the replacement is fitted to its length).
- The movies' audio (`ZMOVIES.HOG`) is out of scope with the movies.

## Sound effects in the banks

Checkpoint 132md5. The short effects (gunfire, impacts, footsteps, the ambient loops, the radio chatter, the
interface's clicks) are SPU ADPCM samples inside the 989snd banks of each `SND.HOG` (TORONTO1: `ENVIRON.BNK` 86
samples, `WEPS.BNK` 100, `VOX.BNK` 22, `SHELL.BNK` 2; the front end's `GAMEDATA/FRONTEND/SND.HOG`: 122). A mod
replaces one sample with a WAV or Ogg Vorbis file below the bank's key, numbered from 0:

```
sounds\GAMEDATA\TORONTO1\SND.HOG\WEPS.BNK\13.wav      sample 13 of TORONTO1's WEPS.BNK (G_BARETTA_92F, G_M14, ...)
```

The entries obey `overrides=` (the sample key, the bank or the HOG), the priorities and the shadowing like every other
entry. **Numbering.** A bank's sounds are made of grains; a grain plays a tone; a tone points at its ADPCM sample in
the bank's body. The samples are the distinct body offsets the tones point at, in ascending order, so a sample several
sounds share (gunfire tails, footsteps on two surfaces) is one file. `omega_mod_tool export-sounds` (or
`mod_export.py sounds MISSION`) writes every sample as `sounds\<bank>\<n>.wav` at its rate, and
`sounds_index.csv` names the sounds that use it (the bank's name table), e.g. `ENVIRON.BNK/23` is `F_STONE_M F_TILE_M`.

**Where it plugs in.** The game loads a bank by copying its body into sound RAM (`SoundIopHost::load_bank`, the
port of 989snd's `snd_BankLoad`). When a mod has entries below that bank's key, the host parses the bank's sample
table, encodes each replacement to PS-ADPCM and attaches it as a port-only *overlay* of the SPU voices
(`Spu2Voices::set_overlay`) at the sample's sound RAM address. A voice keyed on at that address reads the overlay
instead of sound RAM; the bank engine, the game's sound RAM, its pitch, volume, ADSR, pan, reverb and stop commands are
untouched. Unloading the bank (or loading another over it) drops its overlays. The encoded samples are cached per key
for the session. With no `sounds\` entry below a bank nothing is parsed and the disc's samples play.

**Fitting.**

- **Rate.** The game plays a tone at note 60 (middle C) shifted by the sound's own note, bends and pitch; the tone's
  centre note and fine tune say at which note the sample plays at its recorded rate. The replacement is resampled
  (linear) to that rate: `44100 x 2^((60 - centre + fine/128) / 12)` Hz (a negative centre: 48000 Hz and its magnitude),
  e.g. 15989 Hz for TORONTO1's footsteps. So a 1 kHz tone in the WAV is heard at 1 kHz when the game plays the sound at
  note 60, and moves with the game's own pitch changes as the original did (in the evidence below the 990 Hz gunfire
  beep is heard at 1040 Hz and the 1320 Hz footstep at 1260 Hz, the shifts the game gives those sounds). A sample that
  tones of different centres share takes the first tone's.
- **Channels.** Mono (the channels' mean): the SPU voices are mono; the game pans them.
- **Length.** A sample that does not loop is cut, or padded with silence, to the original's length, so the voice
  ends on the same tick as the original's (the bank engine frees and steals voices by their end). A looping sample (its last ADPCM block's loop flag: the ambient beds, the alarms, the generators)
  keeps its own length and loops whole, from its start.
- **Encoding.** The port's PS-ADPCM encoder (`src/port/mods/bank_sounds.cpp`): per 28-sample block, every filter (0-4)
  and shift (0-12) is tried with the decoder's exact arithmetic and the least squared error kept, deterministic (ties:
  the lower filter, then shift). The flags are the original's kind: start, end, and the loop start / repeat for a
  looping sample. A sine round trip is within 1% RMS.

```
Mods (port-only): sound GAMEDATA/TORONTO1/SND.HOG/WEPS.BNK/13 <- md5_bank (13.wav): 22050 Hz x2 0.20 s -> bank sample 53781 Hz x1 0.04 s, 82 ADPCM blocks (the original's length)
Mods (port-only): sound GAMEDATA/TORONTO1/SND.HOG/ENVIRON.BNK/53 <- md5_bank (53.wav): 44100 Hz x1 0.50 s -> bank sample 7995 Hz x1 0.50 s, 143 ADPCM blocks (a looping sample: its own length, looped whole)
Sound: bank GAMEDATA/TORONTO1/SND.HOG/WEPS.BNK: 100 of 100 samples replaced by mods (port-only, sound RAM 0x3d040)
Mods (port-only): sound GAMEDATA/TORONTO1/SND.HOG/WEPS.BNK/0 <- md5_bank_bad (0.wav) refused (WAV: short fmt chunk); the disc's sample is played
Mods (port-only): sound GAMEDATA/TORONTO1/SND.HOG/WEPS.BNK/9999 <- md5_bank_bad (9999.wav) refused (the bank has 100 samples, numbered from 0); not played
```

**Refusals.** A file that does not decode is refused when the bank loads, logged once, and the disc's sample plays. A
number that is not one of the bank's samples is logged once per bank. A whole bank replaced through `files\` by
one mod while another mod replaces samples of it is reported as layered by `omega_mod_tool conflicts`; the sample
numbers are then those of the bank actually loaded. `omega_mod_tool check-sounds` checks bank samples against the
disc's bank: the number, the decode and the fit (the rate, the length or the loop), and lists the sounds that use it.

**Neutrality.** Overlays are port-only state of the SPU voice model: the game's IOP memory and sound RAM are never
written, so the game's own state (and a save) is the same with or without the mod.

### Limits (bank sounds)

- **One rate per sample.** A sample shared by tones with different centre notes is fitted to the first's; the others
  play it shifted as they did the original.
- **No new sounds or grains.** A mod replaces samples; it cannot add sounds, change a sound's grains, its volume
  envelope (ADSR), its pitch or which sample a sound plays (edit the bank through `files\` for that).
- **Loop points.** A looping replacement loops whole; the original's loop start inside the sample is not kept.

## Audio overrides (132md13)

Port-only. The `sounds\` layout (132md4 / 132md5) needs a file per disc key: a level's alarm is a different sample
in every level's `ENVIRON.BNK`, and a music stream sits at one path. A manifest can name the sounds instead and point
them at any file in the mod:

```
audio_overrides=TORONTO1_AMB3.VPK=music/toronto_amb.wav
audio_overrides=ENVIRON.BNK/O_BANK_ALARM=sfx/alarm.ogg, WEPS.BNK/13=sfx/shot.wav
audio_volume=80
```

- **Targets.** Upper-cased, `\` or `/`. A target names a disc sound's key or its end at a `/`:
  - a stream (music, voice): `ZMEDIA/MUSIC.HOG/THEME_MAIN.VPK`, or just `THEME_MAIN.VPK` (every HOG holding it);
  - a bank sample: `GAMEDATA/TORONTO1/SND.HOG/WEPS.BNK/13`, or `WEPS.BNK/13` (sample 13 of every level's
    `WEPS.BNK`); a bare number is refused (it would be every bank's);
  - a bank sound by name: `ENVIRON.BNK/O_BANK_ALARM` (or with its full bank key): every sample that sound plays, in
    every bank that has it (the names are the bank's own table, `sounds_index.csv` of `export-sounds`).
  A target with `*`, `:`, `.` or `..` parts, a file that is not `.wav` / `.ogg`, or a path leaving the mod's folder
  is a bad value (the mod is skipped with the line). A file the mod does not hold is ignored with a log line
  (`<mod>: ignored audio_overrides T=<file> (no such file in the mod)`). The targets are explicit declarations:
  `overrides=` does not filter them.
- **Precedence.** For each disc sound the game plays, the first mod in resolution order that supplies it wins (as for
  every entry). Within a mod, its `sounds\<key>` file comes first, then the target that names the key: a sample
  number over a sound name, then the longer target, then the manifest's order. The same target in two mods: the
  later is shadowed (`conflicts` and `validate` report it, the launcher's conflict list too); a `sounds\` file and
  another mod's target naming the same key are reported as shadowed with the winner.
- **Formats and fitting.** The same as `sounds\`: WAV or Ogg Vorbis, any rate and channel count, decoded by the port
  at load and fitted to the sound it replaces ("Sounds and music", "Sound effects in the banks"): a stream to its
  rate and channels (a VPK 32000 Hz stereo, a VAG 22050 Hz mono), cut or padded to the original's length unless the
  game loops it; a bank sample resampled to its tone's rate, mono, ADPCM-encoded, cut or padded unless it loops. The
  mixer (`audio::Mixer`, 48000 Hz stereo) then plays it as the disc's.
- **Decoded at load.** When the game opens a sound HOG (`003f96e0`), the streams the mods replace in it are decoded
  whole and fitted on a worker thread (`SoundIopHost::preload_mod_streams`, in member order), so the start of a
  replaced music stream reads no file: `Mods (port-only): ZMEDIA\MUSIC.HOG: 1 mod stream(s) decoding at load`. The
  decoded samples are budgeted (64 MB on the PC, 8 MB on the Vita); a stream past the budget, or one that starts
  before its decode ends, is decoded a piece ahead of the mix as in 132md4. Bank samples are encoded when the bank
  loads, as in 132md5.
- **Volume.** `audio_volume=<percent>` (0..400, default 100) scales every sound of the mod (its `sounds\` files
  too), applied to the decoded samples with clamping, before the bank encoding; the game's own volume, pan and fades
  apply after. The player's choice, `mod_volume.<folder>=<percent>` in the settings (`omega_mod_tool volume <mod
  folder> <percent>`; the launcher's details and `list` show it), wins over the manifest. At 100 % the samples are the
  132md4 / 132md5 ones exactly.
- **Shared samples.** A bank sample several sounds play is one sample: naming one sound replaces it for all of them.
  The log, `check-sounds` and `validate` say so (`note: GAMEDATA/ZURICH/SND.HOG/ENVIRON.BNK/37: the sample is
  shared: CLD_ELECTRONIC1 plays it too (replaced for it as well)`).
- **The log.**

```
Mods (port-only): ZMEDIA\MUSIC.HOG: 1 mod stream(s) decoding at load
Mods (port-only): sound GAMEDATA/TORONTO1/SND.HOG/ENVIRON.BNK/38 <- audio_pack (alarm.wav, audio_overrides ENVIRON.BNK/O_BANK_ALARM, volume 80%): 16000 Hz x1 0.50 s -> bank sample 15989 Hz x1 0.50 s, 286 ADPCM blocks (a looping sample: its own length, looped whole)
Sound: bank GAMEDATA/TORONTO1/SND.HOG/ENVIRON.BNK: 1 of 86 samples replaced by mods (port-only, sound RAM 0xe8380)
Mods (port-only): sound ZMEDIA/MUSIC.HOG/TORONTO1_AMB3.VPK <- audio_pack (toronto_amb.wav, audio_overrides TORONTO1_AMB3.VPK, volume 80%): decoded at load -> 32000 Hz x2 2.00 s (a looping stream: its own length)
```

- **The tools.** `check-sounds` and `validate` check each target against the disc (every HOG the game's sound host
  opens: `ZMEDIA\MUSIC.HOG`, `ZMEDIA\SNDVAG.HOG` and each level's `SND.HOG` / `SNDVAG.HOG`, plus a full key's own
  HOG): a target that names nothing or a file that does not decode is refused (validate: `ERROR`); a target whose
  every sound another entry wins is unused (a `warning`); shared samples are notes. `list` prints the targets and the
  volume; `conflicts` the shadowing.
- **On the Vita.** `omega_mod_tool pack --platform vita` converts every sound file the mod holds (`sounds\` and the
  `audio_overrides=` files) to what the game plays it at, so the Vita decodes no Vorbis and resamples nothing at
  load: a 16-bit WAV at the highest rate and channel count among the disc sounds it replaces (a VPK stream 32000 Hz
  stereo; a bank sample its tone's rate, mono), capped at the Vita mixer's 48000 Hz stereo (`SceAudioOut` runs the
  mixer at the PC's rate). No gain is applied (the volume stays a load-time setting). An `.ogg` becomes `.wav` and
  the archive's `audio_overrides=` lines are rewritten to the new names (the stem rule keeps `overrides=` matching).
  A file that replaces no disc sound, or a pack without the disc, keeps the file's own rate (capped). The archive's
  `notes.vita=` (when the mod has none) gains `2 sound(s) as 16-bit WAV at the game's rates (0 OGG decoded; 0.1 MB
  -> 0.3 MB)`. A WAV is larger than an Ogg; the samples a level plays are decoded within the same 8 MB budget either
  way.
- **The example.** `tools\mods\examples\audio_pack` (`python tools\make_example_mod.py --audio-pack` rewrites it
  byte for byte; synthetic): a 2 s ambient loop for `TORONTO1_AMB3.VPK` (the music TORONTO1 starts with) and a
  two-tone alarm for `ENVIRON.BNK/O_BANK_ALARM`, at `audio_volume=80`. `check-sounds --mods tools\mods\examples`:

```
accepted: audio_overrides TORONTO1_AMB3.VPK <- audio_pack (toronto_amb.wav): 16000 Hz x1, 2.00 s, volume 80% -> 1 disc sound(s) named, 1 supplied: ZMEDIA/MUSIC.HOG/TORONTO1_AMB3.VPK (32000 Hz stereo stream, 93.51 s) (fitted to 32000 Hz x2)
accepted: audio_overrides ENVIRON.BNK/O_BANK_ALARM <- audio_pack (alarm.wav): 16000 Hz x1, 0.50 s, volume 80% -> 3 disc sound(s) named, 3 supplied: GAMEDATA/TORONTO1/SND.HOG/ENVIRON.BNK/38 (15989 Hz mono bank sample, looping, 0.60 s); GAMEDATA/ZURICH/SND.HOG/ENVIRON.BNK/37 (22028 Hz mono bank sample, 0.23 s); GAMEDATA/ZURICH/SND.HOG/ENVIRON.BNK/38 (31978 Hz mono bank sample, 0.33 s) (fitted to 15989 Hz x1, 22028 Hz x1, 31978 Hz x1)
  note: GAMEDATA/ZURICH/SND.HOG/ENVIRON.BNK/37: the sample is shared: CLD_ELECTRONIC1 plays it too (replaced for it as well)
2 sound(s): 2 accepted, 0 refused
```

  `pack --platform vita` stores the loop as 32000 Hz stereo and the alarm as 31978 Hz mono (ZURICH's rate, the
  highest of the three).

### Limits (audio overrides)

- **No new sounds.** A target replaces what the disc has; it cannot add a sound or change which sample a sound plays.
- **One file per sample.** A sample shared by several sounds, or matched at several rates, is fitted per rate from
  one file; a target cannot give two sounds of one sample different files.
- **Streams decoded at load** take memory for the level (within the budget); a movie's sound is `movie_overrides=`'s
  ("Movie overrides").

## Movie overrides (132md14)

Port-only. The front end plays the disc's movies (`ZMEDIA\ZMOVIES.HOG`: the intro, the mission briefings and
endings; `FrontendSession`, `003f8220`) as PS2 `.PSS` files: MPEG-2 video and PS2 ADPCM sound, decoded by the
port's own MPEG-2 decoder (`src/lib/libmpeg`) a picture every two fields (30000 / 1001 a second). The `files\`
layout cannot replace them (the movie archive is refused: it is streamed, never loaded whole). A manifest names a
movie and the files that replace it instead:

```
movie_overrides=INTRO=movie/intro_wide.m2v+movie/intro.wav+movie/intro.sub
movie_overrides=TORONTO1_END=movie/toronto_end.ogg
```

- **Targets.** A `ZMOVIES.HOG` member, with or without `.PSS` (`INTRO` and `intro.pss` are `INTRO.PSS`); a name with
  `/`, `\`, `*`, `:` or another extension is a bad value (the mod is skipped with the line), and so is a target given
  twice in one manifest. A mod can only replace a movie the disc has (the session opens only those names);
  `validate` says when a target names none. Several `movie_overrides=` lines accumulate; items are `,`-separated.
- **Files** (paths inside the mod, `+`-separated, at most one of each kind; any may be left out):
  - the **picture**: an MPEG-2 video elementary stream (`.m2v` / `.mpv`: main profile, 4:2:0, frame pictures, up to
    1920 x 1152, any MPEG-2 frame rate and aspect), or a `.pss` in the disc's own format (its sound is used too
    unless the item names one), or (132md15) an `.mp4` / `.m4v` / `.mov` played by the platform's movie decoder
    (H.264 pictures with AAC sound on Windows: "Platform movie decoders" below; its own sound is used unless the item
    names one). None: the disc movie's picture;
  - the **sound**: `.wav` or `.ogg` (any rate and channels, decoded by the port and fitted to 48000 Hz stereo, the
    movie mixer's rate, at the mod's volume: `audio_volume=` / `mod_volume.<folder>=`, "Audio overrides"). None:
    the disc movie's sound (read alongside, for its sound alone, when the picture is the mod's), or the mod `.pss`'s;
  - the **subtitles**: a `.sub` in the disc's format (`%FEED <LANGUAGE>`, `<start> <end> PRINT <position>` and
    quoted lines, `%END`; times `mm:ss:hh` from the movie's start). None: the disc's `.SUB` for the movie.

  A file the mod does not hold drops the item with a log line (`<mod>: ignored movie_overrides INTRO.PSS (no file
  movie/intro.wav in the mod)`). The same movie from two mods: the first in resolution order wins, the other is
  shadowed (`conflicts`, `validate`, the launcher and the log report it). The targets are explicit declarations:
  `overrides=` does not filter them.
- **MPEG-2 or MP4.** MPEG-2 is what the port decodes itself (the disc's movies), on every platform, with no
  dependency. ffmpeg writes it: `ffmpeg -i remaster.mp4 -an -c:v mpeg2video -q:v 2 -g 15 -bf 2 movie/intro.m2v` and
  `ffmpeg -i remaster.mp4 -vn -ac 2 -ar 48000 movie/intro.wav` (or `-c:a libvorbis movie/intro.ogg`). An H.264 / AAC
  MP4 (132md15) is a fraction of the size at the same quality and needs no conversion, but plays only where the
  platform has a movie decoder: Windows (Media Foundation, a system component) and, once its host installs one, the
  Vita (`sceAvPlayer`); elsewhere the disc's movie plays. A mod for every platform ships MPEG-2.
- **Timing.** The session's clock is kept: it pulls a picture every two fields and counts the fields for the
  subtitles, the skip button, the end message and the level-start hand-off, as for a disc movie. At the k-th pull the
  replacement shows its source picture `floor(k x fps x 1001 / 30000)` (23.976 / 25 fps pictures repeat, 50 / 59.94
  fps ones are dropped), so the movie lasts as long as its stream at any frame rate, and the sound is handed out on
  the same clock (through pull k, `(k + 1) x 48000 x 1001 / 30000` frames), so it stays in step with the picture. The
  movie ends when its picture ends (a sound longer than the picture is cut; a shorter one leaves silence). A level's
  start trigger after its briefing movie is the session's, so a longer or shorter movie only moves it.
- **Aspect.** A 4:3 picture fills the screen as the disc's movies do. A wider picture (MPEG-2 aspect 16:9 or 2.21:1,
  or square pixels wider than 4:3) is letterboxed in the 4:3 frame on a 4:3 target. On a wide target (`aspect=wide`,
  132ws) it is drawn at its own aspect across the target's width, letterboxed only when wider than the target; one
  narrower than the target is as tall as the target and pillarboxed with `movie_fill=bars`, and keeps the width (top
  and bottom cut) with `movie_fill=zoom`, as the disc's movies do (`GuiDrawBatch::port_picture_aspect`, the
  renderer's `wide_aspect_picture` range). A picture narrower than 4:3 is pillarboxed. The subtitles stay where the
  game puts them.
- **Decoding.** The picture is decoded on a worker thread a few pictures ahead (a bounded queue: four frames, so a
  1920 x 1080 movie holds about 12 MB); a stored member of a mod archive is streamed from the `.zip` in place, a
  deflated one is read whole first (132md15: `pack` always stores movie files, `.m2v` / `.mpv` / `.pss` / `.mp4` /
  `.m4v` / `.mov`, so every packed movie streams). The sound is decoded whole when the movie
  opens. A picture that fails to decode ends the movie there (logged); a movie that cannot be opened at all plays the
  disc's (logged).
- **The log.**

```
Mods (port-only): movie INTRO.PSS <- movie_pack: picture intro_wide.m2v 256x144 23.976 fps 16:9, sound intro.wav
Mods (port-only): movie subtitles INTRO.SUB <- movie_pack: intro.sub
```

- **The tools.** `list` and `conflicts` show the targets (kind `movies`). `validate` decodes each movie as the game
  would: an unknown target, a picture that does not decode, a sound that does not decode or a `.sub` that does not
  parse is an `ERROR`; a sound whose length differs from the picture's by more than a second, or subtitles with no
  feed, a `warning`; the picture's size, aspect, frame rate and length (against the disc movie's) are notes.
  `pack --platform vita` keeps movies as they are (the Vita runs the same decoder in software) and notes them in
  `notes.vita=`; a picture larger than the Vita's 960 x 544 screen is a warning (it decodes, slowly).
- **The example.** `tools\mods\examples\movie_pack` (`python tools\make_example_mod.py --movie-pack` rewrites it
  byte for byte; synthetic): a 3 s 256 x 144 16:9 23.976 fps stand-in for `INTRO.PSS` (72 intra pictures written by
  the script's own minimal MPEG-2 encoder: each 8 x 8 block one flat colour), a three-tone chord as its sound and one
  English subtitle cue.

### Limits (movie overrides)

- **Existing movies only.** A target replaces a `ZMOVIES.HOG` movie; a mod cannot add a movie or change when one plays.
- **MPEG-2, PSS or (with a platform decoder) MP4 pictures.** Other codecs need converting (above); interlaced MPEG-2
  field pictures are not decoded.

### Platform movie decoders (132md15)

Port-only. A movie override's picture named `.mp4`, `.m4v` or `.mov` is decoded by the platform's own decoder, behind
the same `movie::MovieStream` the session plays (`mods::ModMovie`), so the session, its clock, subtitles, skip button,
end message and level-start hand-off are those of every other movie ("Timing" above).

- **The interface** (`src/port/platform/platform_movie.hpp`, namespace `omega::movie`):
  - `MovieFile{name, size, read(offset, span)}`: the file, read at any offset (a plain file, or a member of a mod
    archive streamed in place: `mods::mod_movie_file`);
  - `PlatformMovieBackend{name, extensions, open}`: `open(MovieFile)` returns a `PlatformMovieDecoder` or throws
    `std::runtime_error` naming why not (not a movie, no decoder for the codec);
  - `PlatformMovieDecoder`: `info()` (shown size with the decoder's padding cropped, display aspect with the pixel
    aspect applied, nominal frame rate, duration, sound rate and channels, codec names), `next_picture(Frame&,
    seconds)` (the next picture in display order, I420 `mpeg2::Frame`, with its presentation time), `next_audio(pcm,
    seconds)` (the next piece of interleaved int16 sound with its first sample's time), `error()`. Every call, the
    destruction included, is made on the thread that opened it, so a backend needs no locking of its own;
  - `set_platform_movie_backend(backend)` installs one (a host at start-up; tests), `platform_movie_backend()` is the
    one in use (the installed one, else the platform's default: Media Foundation on Windows, else none).
- **What the port does with it** (`mods::PlatformVideo`, `mod_movie.cpp`): opens the decoder on its own worker thread
  and keeps four pictures ahead; the k-th pull of the front end's clock (`k x 1001 / 30000` s) shows the last picture
  whose time has come (a 24 fps movie repeats pictures, a 60 fps one drops them, by their times); the times count
  from the first picture. The sound is aligned to the first picture (silence before a late start, samples dropped
  before an early one), its first two channels mixed to stereo, resampled to 48000 Hz and handed out on the same
  clock as for an MPEG-2 movie, at the mod's volume. The movie ends one picture interval after its last picture.
- **Windows: Media Foundation** (`platform_movie_mf.cpp`, built when `OMEGA_MEDIA_FOUNDATION` is defined: every
  Windows build; it links `mfplat`, `mfreadwrite`, `mfuuid`, `ole32` and `propsys`, all system libraries, so nothing
  is shipped). A Source Reader over an `IStream` that reads the `MovieFile`, with the system's decoders: H.264 (any
  profile Windows decodes; HEVC where its extension is installed) to NV12 or I420, AAC (or MP3, or PCM) to 16-bit PCM.
  The video's minimum display aperture and pixel aspect are applied. Pictures larger than 1920 x 1152 (what the front
  end shows) are refused.
- **When it cannot play.** No backend on the platform, a file the decoder refuses, a codec it lacks or a size past
  1920 x 1152: the disc's movie plays, with the line `Mods (port-only): movie INTRO.PSS from <mod> not played
  (<why>); the disc's plays`. A decode error mid-movie ends the movie there (logged), as for MPEG-2.
- **The log.**

```
Mods (port-only): movie INTRO.PSS <- mp4_intro: picture intro.mp4 320x180 H.264 24.000 fps 16:9 (Media Foundation), sound intro.mp4's own (AAC 44100 Hz x2)
```

- **The tools.** `validate` opens the MP4 through the same backend and reads it to the end (size, codec, frame rate,
  picture count, length; the sound's codec, rate, channels and length): a file the backend refuses is an `ERROR`,
  and so is an MP4 on a platform with no backend. `pack --platform vita` keeps it as it is with a note in
  `notes.vita=` ("played on the Vita only through its sceAvPlayer movie backend ... else the disc's movie plays").
  Movies are always stored in an archive (never deflated: already compressed, and streamed in place).

### The Vita's movie backend

For the Vita host (agent 132vt; nothing in the Vita harness is changed by this stage). The Vita plays H.264 / AAC in
hardware through `sceAvPlayer`; a backend for it implements `PlatformMovieBackend` and the host installs it at
start-up with `omega::movie::set_platform_movie_backend(...)`, before the front end opens a movie. The mapping:

- `extensions`: `{".mp4", ".m4v"}` (what `sceAvPlayer` reads); `name`: `"sceAvPlayer"`.
- `open(MovieFile file)`: `sceAvPlayerInit` with `SceAvPlayerInitData.fileReplacement` set to callbacks over `file`:
  `open` succeeds, `size` returns `file.size`, `readOffset(argP, buffer, position, length)` returns `file.read(position,
  {buffer, length})`, `close` does nothing (the `MovieFile` lives in the decoder). Then `sceAvPlayerAddSource` with
  `file.name`, wait for the streams (`sceAvPlayerStreamCount`, `sceAvPlayerGetStreamInfo`) and fill `info()` from the
  video stream's width, height (cropped to its display size) and aspect, and the audio stream's rate and channels.
  Throw `std::runtime_error` when no video stream appears.
- `next_picture(frame, seconds)`: `sceAvPlayerGetVideoData`; its NV12 picture (`SceAvPlayerFrameInfo.pData`, the
  luma plane then interleaved chroma) is copied into `frame`'s Y, Cb and Cr planes (I420, chroma at half size each
  way), `seconds = timeStamp / 1000.0`. Return false when `sceAvPlayerIsActive` is false and no frame came.
- `next_audio(pcm, seconds)`: `sceAvPlayerGetAudioData`; append its interleaved int16 samples, `seconds =
  timeStamp / 1000.0`. False at the end of the sound.
- Pacing: `sceAvPlayer` paces on its own clock; the port pulls pictures as fast as the four-picture queue allows and
  paces them itself by their times, so the player should be driven so that `GetVideoData` returns the next picture
  whenever one is decoded (if the player only hands out pictures in real time, the queue simply stays shorter; the
  times still place them right).
- Memory: the 64 MB texture cap does not cover movies; a 960 x 544 picture queue is about 3 MB. A larger MP4 decodes
  (up to the hardware's 1920 x 1088) but costs more; `pack --platform vita` says so in its note.
- Threads: all calls come from `mods::PlatformVideo`'s worker thread (one per playing movie, made and joined by the
  front end's movie session), never from the render thread.

## Gameplay data overrides (132md16)

Port-only. A mod may change the game's tuning tables by field name: the weapons (damage, accuracy, ranges, clip,
rate, noise), each level's NPC health and armor, and the shooters' aim factors (`game::ToHit`). The manifest names
INI files with `data_overrides=` (a comma list, like `audio_overrides=`):

```
name=Stronger silenced pistol
enabled=0
data_overrides=pistol.ini
```

```ini
# pistol.ini
[weapon SocomSilenced]
damage_near = 150
damage_mid = 150
damage_far = 150
clip = 15
```

- **The tables** (`src/port/mods/data_overrides.*`; every field, its offset, range and meaning is in the field table
  there, and `omega_mod_tool validate` lists them against the disc):

  | Section | Table | Fields |
  |---|---|---|
  | `[weapon <id or display key>]` | `GAMEDATA\COMMON\WEAPONS.WDB`, the weapon database (loader 00147400; a 0x10-byte header, 0x140-byte records; [COMBAT.md](COMBAT.md) §1, [WEAPONS.md](WEAPONS.md)) | `range_near/mid/far`, `accuracy_near/mid/far`, `aim_time_near/far`, `burst`, `damage_near/mid/far`, `clip`, `clips`, `full_auto`, `tracer_interval`, `rate`, `noise_radius`, `explosion_radius`, `explosion_outer`, `penetration`, `impact_noise`, `power` |
  | `[npc <LEVEL> <id>]`, `[npc <LEVEL> player]` | `GAMEDATA\<LEVEL>\DATA.POP`'s NPC records (001b6678): the two parameter words | `health`, `armor` |
  | `[npc <LEVEL> *]`, `[npc * *]` | every AI NPC of the level, or of every level (the player's record excluded) | `health_percent`, `armor_percent` |
  | `[tohit]` | `game::ToHit`, the ToHitSettings static at 0048ac80 (in the program image, not on the disc) | the script's names: `runAccFactor`, `runAimFactor`, `kneelAccFactor`, `kneelAimFactor`, `movingTargAccFactor`, `crouchTargAccFactor`, `rollTargAccFactor`, `maxVelocity`, `npcExtraAimTime`, `npcMovingTargVal`, `npcRollingTargVal`, `reloadAimLoss`, `npcNearDist`, `npcNearMul` |

  A weapon is named by its record id (the number `--loadout` and the play log use: 106 is the Beretta, 112 the
  silenced pistol) or its display key (`SocomSilenced`, `BARETTA_92F`; a key several records share changes every
  one). NPC ids are the play log's (`Hit at frame N: 133140 ...`).
- **Values are what the game uses.** A weapon whose damage type (+0xbc) is 1 has its damage words divided by 5 at
  load (00147400); the port stores five times the value given, so `damage_near = 150` means 150 per hit either way.
  Every value is range-checked (the clip 1-999, a percentage 0-100, a damage 0-100000, ...); a malformed line, an
  unknown section or field, a field a section does not take, a value of the wrong type or out of range, or a field
  given twice in a section is an error naming the file and line; that file is ignored (the log says so, e.g.
  `stronger_silenced_pistol: ignored data_overrides pistol.ini (pistol.ini line 4: ...)`) and the mod's other files
  still apply.
- **How it applies.** The disc layer serves the patched bytes of `WEAPONS.WDB` and each `DATA.POP` (a `files\`
  replacement of the same file first, then the fields) to every reader: the play, the front end's loadout and award
  screens. `[tohit]` sets the play's combat aim and the mission scripts' `game::ToHit` at each level start. Each
  patched file logs one line: `Mods (port-only): data: WEAPONS.WDB: 4 field(s) from stronger_silenced_pistol`.
- **Resolution.** Each field (a section and a field: `weapon SOCOMSILENCED damage_near`) is an entry like a file:
  the first mod in resolution order wins it, a later one is shadowed (the log and the launcher's conflict list say
  which). For one NPC, its own `[npc <LEVEL> <id>]` value wins over `[npc <LEVEL> *]`'s percentage, which wins over
  `[npc * *]`'s; they do not compound (a percentage scales the disc's value, rounded, at least 1 health).
- **Gameplay mods.** A mod with `data_overrides=` changes gameplay (the class `gameplay` of "Online: the mod report"),
  so the examples ship it `enabled=0`: enable it on the MODS page or with `enabled=1`. The LAN host and omega_server
  default to the **gameplay** policy: a joiner's gameplay set (the gameplay fingerprint: every gameplay-class `files\`
  entry with its bytes' hash and every data field with its value) must equal the host's, the game creator's online;
  cosmetic mods (textures, models, sounds, movies, fonts) may differ. Without gameplay mods on either side nothing
  changes. A refused joiner sees *Your gameplay mods differ from the host's (yours 22222222, the host's 11111111)*.
  The other policies (`any`, `cosmetic`, `none`, LAN's `equal`) are as before.
- **Not provided, and why.**
  - *Difficulty tables:* the game has none ([ACTOR_DAMAGE_AND_HEALTH.md](ACTOR_DAMAGE_AND_HEALTH.md)); `[npc * *]`'s
    `health_percent` / `armor_percent` and the weapons' `accuracy_*` are the knobs for one.
  - *Engine constants* (the player's starting armor, gravity, the camera's): they are constants in the program, not
    tables, and the port keeps them as C++ constants.
  - *An ammo table:* ammunition is the weapon table's `clip` and `clips` (the clips a fresh pickup or loadout
    carries); there is no separate one.
- **Mid-run changes.** A gameplay set, like any mod set, changes at the next level ("Mod changes at the next level"),
  never in a run that opened a LAN or online session.

### Limits (data overrides)

- Fields outside the table are not offered; adding one is a line in `data_overrides.cpp` with its offset and range,
  once the field is labelled in the disassembly.
- The report is self-declared (as for every mod report): the gameplay policy keeps honest players with different
  tuning apart; a modified client can lie.

## Interface art and fonts

Checkpoint 132md4. The front end's art, the in-game menus, the fonts, the loading screens and the error
screen are TDX textures (`COMMON/FONTS.HOG`, `COMMON/LOADING.HOG`, `COMMON/ERROR.TDX`, `FRONTEND/NTSC.HOG` and its
nested HOGs, e.g. `GAMEGUI.HOG`, `INGAMENU.HOG`), decoded by `decode_tdx` like every other texture. So `textures\` entries replace them
with no code of their own: PNG / DDS at any size, the alpha rules of "Texture semantics", one image per animation
frame. `tools/mod_export.py gui` exports them.

- **Fonts.** A font page (`FONTDEFA.TDX`, `FONTCALL.TDX`, `FONTLARG.TDX`) is a texture; the glyph rectangles,
  advances and kerning are in the `.FNT` members and are unchanged. UVs are normalised to the original's size, so a
  2× or 4× page draws every glyph at the same place and size with more detail. Keep each glyph inside its original
  cell (scale the whole page; do not move glyphs).
- **Shared images.** Many interface images are the same pixels in several nested HOGs (a bracket or a backdrop in
  each menu's archive). A path key replaces every texture with that original image, and when two path keys share
  one, the first registered wins with a log line (`... has the same original image as ...; using ...`), as for any
  texture.
- **Text.** The strings (`STRINGS.DAT`) and the font metrics (`.FNT`) are data, replaceable as `files\` entries in
  the game's own format; the menus' layout is in the executable and is not.
- **The GUI samples level 0 only** (no mips), as for the originals: a page much larger than its drawn size is
  minified bilinearly.

## Evidence (132md3, face and hand shapes)

- **Core tests** (`tests/model_tests.cpp`, 2 groups):
  - morph targets: names read from `targetNames`, only moved vertices kept, in game space; `NAME@index` naming and
    binding; an unknown name and two targets on one shape refused; weighted offsets in slot order before the
    palette; zero weights and unbound shapes leave the plain skinning; the three load refusals (no names, a name
    twice, a size mismatch); the overrides binding each part to its table, a part whose table lacks a target
    refused with the fallback, and targets on a part without shapes ignored (each with its log line);
  - the disc round trip (skipped without the ISO): 3 face and 3 hand `MORPH.HOG` parts export with 304 targets and
    reload. Deformed by the same shape slots (up to 4 weighted shapes, which move the original by up to 0.41) under
    a posed palette, the replacement matches the original's morphed skinning with a worst deviation of 0.
- **Validator:** the 20 PC_MALE exports (head 88 face targets, hands 11 and 17 hand targets) and the generated
  test mods report 0 errors and 0 warnings.
- **Neutrality:** with no mods, the 8 play regressions are Identical, and the restart and level-end chain are
  unchanged. A 120-frame capture gives the baseline's frame, shadow and trace.
- **In play** (`tools\make_test_mods.py --morph`; hidden `--textured` TORONTO1 objectives route, 2000 frames with
  combat):
  - `local\test_mods\morph`: the PC_MALE head (green, its texture at 4×), mouth and neck are scaled 1.1 about the
    head, and the hands are subdivided twice (165 → 2640 triangles) and green, each with its targets (interpolated
    at the new vertices). Two refusal parts (`md3_morph_bad`) are logged as above.
  - The log shows the player's hands drawn with `CGRIP` / `ET1` and his face with `EMO_SHOT`, then an NPC's face
    with its base shape `SHAPE_KHORSH` and hands with `AGRIP` / `ET2`.
  - `local\test_mods\morph_flat` is the same mod with the hands' targets removed. Its capture at frame 1990 has the
    same trace and frame, but a different character shadow buffer: the grip shapes move the replaced hands.

## Evidence (132md3, front-end figures)

With `tools\make_test_mods.py --models` (`local\test_mods\models`: the green hair at 4×, the female agent's green
hair and inflated shirt; `md2_bad`'s refusals):

- **The loadout screen** (`tools\run_toronto1_level_end.ps1 -CallFrame 0 -Frames 220 -Script toronto1_loadout.txt
  -Loadout none -Extra @('--gui-overlay', '--mods', ..., '--capture', '100', '--capture', '200')`): the memory card
  agent's `1HAIR_A03_HAIR0` is replaced (`1 replaced by mods`) and the boots / trousers are refused for `MALE.SKEL`.
  Frames 100 and 200 differ from the no-mods run only in the figure's hair (a 51×45 box): green, on the figure's
  head.
- **The front end** (`-Female`, the NEW AGENT walk): the male default agent at frame 405 (3863 triangles against
  2333: the subdivided hair) and the female agent at 559 / 721 (shirt, then hair) draw the replacements, validated
  against `FRONTEND/MALE.SKEL` and `FRONTEND/FEMALE.SKEL`. Of the front end's captures only `AGENTNEW` and
  `KEYBOARD` (the figures) differ from the no-mods run; the title, the save, ONOFFSEL and CMDCENTR are identical.
  The loading screen's light streak differs too, as it does between two no-mods runs (it varies from run to run).
- **Neutrality:** with no mods the 8 play regressions are Identical, and the restart and level-end chain are
  unchanged.

## Evidence (132md3, gun models)

- **Core tests** (`tests/model_tests.cpp`, 2 groups):
  - the gun skeleton (synthetic): the name, the parents and the bind pose from the model bytes, a parent not below
    its bone and an undefined byte refused; the exported skeleton validates, a character's skeleton, a moved bone
    and a re-hung bone refuse it; a replacement loads against it; skinning by posed slot matrices places each
    vertex and normal by its bone's matrix;
  - the disc round trip (skipped without the ISO): 49 parts (12 flare) of 16 COMMON guns export rigged to their
    gun's bones and reload; skinned by posed slot matrices, they match the original parts' triangles (worst
    deviation 4.8e-7) and UVs. Then the lookup: no overrides without mods, COMMON's key replacing a part (LOD 1
    sharing it, logged), a mission key rigged to another skeleton taking precedence and refused with the fallback
    logged, nothing after uninstalling.
- **Validator:** 60 COMMON exports and 25 generated test parts report 0 errors.
- **Loadout screen** (`run_toronto1_level_end.ps1 -CallFrame 0 -Frames 220 -Script toronto1_loadout.txt -Loadout
  none -Extra @('--gui-overlay', '--mods', 'local\test_mods\guns', '--capture', '100', '--capture', '200')`): the
  held 92F and both weapon previews are green and 1.3× larger; frames 100 and 200 differ from the no-mods run only in
  the guns (the figure's hand and the two preview panels).
- **In play** (`run_play_scratch.ps1 -Frames 300` with `--textured --gui-overlay --combat --mission-scripts
  --npc-perception --hard-mesh 1HARD_DEF_V02-0.SKM --loadout default --capture 290`): the holstered 92F and the HUD
  preview are green and larger; the flashlight is refused (bind moved) and drawn as the disc's. The trace and the
  shadow buffer equal the no-mods run's; only the frame differs.
- **Neutrality:** with no mods the 8 play regressions are Identical, and the restart and level-end chain are
  unchanged.

## Evidence (132md3, colour patches and animated materials)

- **Core tests** (`tests/model_tests.cpp`):
  - the remap and the layout (synthetic): a re-export keeps each set's off state at every vertex; nearest vertices
    with the colour ratio (16 × 255 / 96 → 43) and the clamp (a darkening at 0xff gives no change); three animated
    surfaces of two materials laid out in order, the first holding its triangles; the animators equal the
    original's; an animated material's image ignored with its note; a hidden replacement keeping the animators on
    empty surfaces; an unnamed primitive joining the first animated material; the `COLOR_0` clamp at 0xff;
  - the disc round trip (skipped without the ISO; it now walks every TORONTO1 model and checks the first 12 plain
    ones per archive exactly): all 15 colour-patched models (14031 vertices patched) and all 13 animated models
    (13 animators) reload with every set's off state equal at every vertex and the original's animators.
- **Lamps switching in play** (`run_play_scratch.ps1 -Frames 520 -Script toronto1_colour_patch.txt` with `--start
  -600 -10 0.5 180 --mission-scripts --textured --capture 190 --capture 350 --capture 510`, with and without `--mods
  local\test_mods\refused`):
  - Both runs make the same 24 walks off at frame 200 and on at frame 360.
  - With the mod they change the replacement's vertices, for example `TORONTO_DOWNTOWN_A07 set 4 off: 138 vertex
    colours changed` against the disc's 65.
  - The trace (simulation and the game's random numbers) is equal; the frames show the replaced street green.
  - The mod log has 28 replacements (15 with their patch sets, 13 with their animated surface, the 13 ignored images
    logged).
  - `Material animators (00376ad0): 5 UV scroll, 31 texture frame` and the 60 colour patch sets in 10 PNT lights'
    slots are the same as without mods.
- **Captures** (`--textured --capture 120`): at `--start -44.2 109.66 8 -90`, the hallway piece `A26` is green, while
  its animated sign keeps the disc's texture. At `--start -600 -15 1 90`, under the A07 street lamps, the colour-patched street is green. In
  both, the trace and the shadow buffer equal the no-mods run's.
- **Neutrality:** with no mods the 8 play regressions are Identical, and the restart and level-end chain are
  unchanged.

## Evidence (132md4, every mission)

- **The sweep** (`tools/level_model_sweep.py`, the table in "Level models in every mission"): 7036 level models of
  the 18 missions export and load back, 0 refused, 0 skipped (exit 0).
- **TORONTO2** (`make_test_mods.py --mission TORONTO2`: its 32 colour-patched and 12 animated models, subdivided, red
  and blue at 0.6, textures green; its 3 map models red): `check-models` accepts all 43. A hidden `--textured`
  capture at frame 130 from `--start -60 -16 -52 90` (in `TORONTO_SEWERS_A24`, 21 colour patch sets) shows the sewer
  green against the baseline's grey. All 40 level replacements are logged with
  their patch sets (`TORONTO_SEWERS_A24 ... 21 colour patch sets on 5554 vertices`) and 170 materials draw their
  glTF images.
- **The HUD map** (TORONTO1, `make_test_mods.py --mission TORONTO1`, `tools/play_scripts/toronto1_map.txt` with
  `--mission-scripts --combat --gui-overlay --textured`): the map opens at frame 91 and draws 9524 triangles of the
  2 replaced models (the disc's: 2381), orange (the red `COLOR_0` on the green texture) where the baseline's are
  green, at the same place and extent, the player arrow and the markers unchanged.
- **Neutrality:** with no mods the 8 play regressions are Identical (the map's override lookup returns nothing
  without a mod set).

## Evidence (132md4, managing mods)

- **Core tests:** `test_mod_management` (`tests/mod_tests.cpp`): `overrides=` parsing and its errors,
  `manifest_declares`, the listing's order (priority, name, invalid folders last), the settings' choices over the
  manifests (both log lines), undeclared entries ignored, `ModSet::conflicts` (one shadowed, one layered) and
  `mod_choices_setting`. `test_mods_page` (`tests/port_settings_page_tests.cpp`): the MODS rows, the toggles writing
  `mod.<folder>=` and `mods=`, `INVALID`, `(NEXT)` and the `MORE MODS` paging of 9 mods.
- **Stacking:** the md1 test mods (`make_test_mods.py --out local\test_mods\md4_manage`) with `declare` run on two of
  them and a fifth, `md4_layer` (priority 1, `overrides=GAMEDATA/COMMON/MANTEX.HOG`: yellow trousers over
  `md1_files`' replaced trousers member, and one undeclared TORONTO1 texture). `omega_mod_tool conflicts` reports 1
  shadowed (the vest, `md1_recolour` over `md1_fallback`), 1 layered (`md4_layer` over `md1_files`) and 1 ignored.
  `disable md1_recolour` and `enable md1_disabled` write `mod.md1_recolour=0` / `mod.md1_disabled=1`; `list` then
  shows the new order (`md1_disabled (100) md1_files (5) md4_layer (1) md1_fallback (0)`).
- **In play:** hidden `--textured` TORONTO1 captures at frame 60 with `--mods`: without a settings file the manifests
  decide (the level textures R/B swapped, the vest red); with that settings file (`--port-settings`) the log shows
  both choices, the level textures are the disc's and the vest is `md1_disabled`'s white. Both draw the yellow
  trousers (the layered mod).
- **The MODS page:** `tools/play_scripts/toronto1_port_settings_mods.txt` (`run_toronto1_level_end.ps1 -CallFrame 0`,
  `--gui-overlay`) opens the page from the pause menu, `L1` to MODS (5 mods in resolution order, the settings'
  choices shown) and toggles the first: `mod.md1_disabled=0` written, the row `OFF (NEXT)`.
- **Neutrality:** with no mods the 8 play regressions are Identical; hidden runs without `--port-settings` read no
  settings, as before.

## Evidence (132md4, sounds and music)

- **Core tests:** `test_mod_sound_files` (`tests/mod_audio_tests.cpp`): WAV (PCM 8 / 16 / 24 / 32, float,
  extensible) and Ogg Vorbis (mono and stereo, within 1 LSB of the test vectors' libvorbisfile decode,
  `mod_audio_vectors.hpp`, and exactly its length), fitting (channels, resampling, cut, pad, loop), `FittedAudio` decoded in uneven pieces
  equal to the whole-file fit, and the refusals (a page CRC, truncation, not a WAV). `test_mod_sound_entries`
  (the same file): the `sounds\` keys, `overrides=`, shadowing and the ignored entries (an `.mp3`, a sound outside
  an archive).
- **In play and the front end:** `run_toronto1_level_end.ps1` (1500 frames) with `--audio-wav` and
  `--frontend-audio-wav` (the headless 48 kHz mixes) and `make_test_mods.py --sounds`:
  - two runs without mods give bit-identical mixes;
  - with `md4_sounds`, the three streams are logged as replaced (above). The play mix (12.0 s) differs from the baseline
    throughout: the 880 Hz beeps for the first 4 s (the voice line, padded with silence to its 13.48 s) over the
    looping A chord (110 Hz dominant). The front-end mix differs over 3.35–15.2 s and 24.65–40.3 s, where the title
    theme plays: the C major chord (261 Hz dominant), looping every 5 s;
  - with `md4_sounds_bad`, the `.mp3` is ignored and the theme refused (logged); both mixes are bit-identical to the
    baseline;
  - in every run the game's log is the baseline's but for the `Mods` lines and the wall-clock / window lines, and
    `trace.json` and every capture are identical (the subtitles and the scripts' timing are unchanged): the
    replacement is fitted to the voice line's length;
  - the progressive decode gives the same mixes, bit for bit, as decoding each file whole at its start.
- **The tool:** `check-sounds` accepts the 3 sounds of `md4_sounds` (exit 0) and refuses `md4_sounds_bad`'s theme
  with the play's reason (exit 1). `mod_export.py sounds TORONTO1` exports the streams of its four archives (among them 114
  voice lines in TORONTO1's `SNDVAG.HOG` and 85 music streams in `MUSIC.HOG`).
- **Neutrality:** with no mods the 8 play regressions are Identical and the restart and level-end chain are
  unchanged.

## Evidence (132md4, interface art and fonts)

- **The mod:** `make_test_mods.py --gui`: 3 font pages yellow at 2× and 358 interface images cyan at 2× with a grid
  (361 files).
- **The front end:** `run_toronto1_level_end.ps1 --mods local\test_mods\gui`: 90 interface textures are replaced as
  the front end loads them (logged with their sizes, e.g. `FONTDEFA.TDX <- md4_gui (256x256 -> 512x512)`), plus 209
  shared-image lines; no entry fails. 13 of the 14 front-end captures differ from the baseline (the title, the
  agent and online menus, the command centre, the movie subtitles and the memory card screen: cyan art, yellow text);
  the intro movie frame, which draws no interface, is identical. The play's `trace.json` is identical and the game's
  log is the baseline's but for the `Mods` and wall-clock lines.
- **Glyph metrics:** in the TORONTO1 start movie's subtitle ("Carthage, Michigan", 2× yellow font) the text's
  bounding box is the baseline's (x 20–186, y 412–424) with the same 17 glyph runs at the same columns, ±1 pixel at
  the antialiased edges.

## Evidence (132md5, bank sound effects)

- **Core tests:** `test_mod_bank_sounds` (`tests/mod_bank_sound_tests.cpp`), on synthetic vectors only: a bank built
  in the test (3 sounds, 5 grains, 3 tones of centre notes 60, 72 and -60, a name table, a sample two sounds share, a
  looping one) gives the sample table, rates, loop flags and sound names expected, and the refusals (not a bank, a
  truncated body, a sample with no end block, a tone outside the body); the encoder reproduces a constant exactly, a sine within 1% RMS, is deterministic and
  writes the original's flags; Spu2Voices plays an overlay in place of sound RAM, loops it and drops it on
  `clear_overlays`; the `sounds\` keys below a bank obey `overrides=`.
- **The tool:** `export-sounds GAMEDATA/TORONTO1/SND.HOG` writes 210 samples (ENVIRON 86 of them 14 looping, WEPS
  100 / 4, VOX 22, SHELL 2) in about a second; `check-sounds` accepts the 208 samples of `md5_bank`
  (`make_test_mods.py --bank-sounds`) and refuses `md5_bank_bad`'s two with the play's reasons (exit 1); its rates match
  the play's for all 208.
- **In play:** `run_toronto1_level_end.ps1` with `--audio-wav` (1500 frames, and `--script run_fire.txt`: walk and
  fire from the subway start):
  - two runs without mods, and two with `md5_bank_bad`, give bit-identical mixes (the baseline's hash is md4's);
  - with `md5_bank` (every TORONTO1 ENVIRON / WEPS / VOX sample a synthetic beep or loop) the three banks log
    100 / 86 / 22 of their samples replaced and the mix differs: at the first G_BARETTA_92F the difference is 1040 Hz
    (the 990 Hz WEPS beep), at the first F_STONE_M footstep 1260 Hz (the 1320 Hz ENVIRON beep), between the shots
    310 Hz (the 330 Hz loops); two runs are bit-identical;
  - the front end's mix is unchanged (its banks are `GAMEDATA/FRONTEND/SND.HOG`'s, which the test mod does not name).
- **Neutrality:** with no mods the 8 play regressions are Identical and the restart and level-end chain are
  unchanged.

## Online: the mod report

Checkpoint 132md4. Online, the port tells omega_server which mods it runs, and the server can refuse games from
modded clients. The wire format and the server side are in [ONLINE.md](ONLINE.md) §6.9; this is the mod side
(`src/port/mods/mod_fingerprint.*`).

- **Classes.** Each entry of an enabled mod is cosmetic or gameplay:

  | Entry | Class |
  |---|---|
  | `textures\`, `models\`, `sounds\` | cosmetic |
  | `files\` with `.TDX`, `.FNT`, `.VAG` or `.VPK` (a texture, a font, a voice or music stream) | cosmetic |
  | any other `files\` entry: scripts, populations, skeletons, appearance manifests, level data, a whole `.HOG` | gameplay |
  | a `data_overrides=` field (132md16) | gameplay |

  The report's `mod_class` is `none` without enabled mods, `gameplay` when any entry is gameplay, else `cosmetic`.
  The counts are of the asset keys the enabled mods override (a key several mods supply counts once; entries
  outside a mod's `overrides=` are ignored, as in play).
- **Fingerprint.** `mod_fingerprint` is a SHA-256 over `mod <FOLDER>` and then every file of each enabled mod
  (its relative path with `/`, sorted, and the SHA-256 of its bytes; the manifest included), the mods in
  resolution order. It changes when any file, the load order or the enabled set changes, and is the same on two
  machines with the same mod folders.
  It is empty with no mods.
- **When.** The port's lobby sends the report after its Hello, once per connection; it is computed when the mods
  are installed and recomputed after a change.
- **The server.** `mod_policy=gameplay` (the default since 132md16) lets a player join a game only when its gameplay
  set (the report's gameplay fingerprint) is the creator's; cosmetic mods may differ, and creating is free. `any`
  allows everything; `cosmetic` refuses creating or joining a game with a gameplay mod; `none` refuses any mod. The
  refused player sees the server's notice. An original (PS2) session and a port session without a report count as
  having no gameplay mods ("Gameplay data overrides").
- **Limits.** The report is self-declared, not anti-cheat. "Cosmetic" is not "fair": a texture or a model can make
  something easier to see or hide, so a strict server uses `none`. The LAN game (no omega_server) has its own
  check at join: "LAN: the mod report at join".

## Evidence (132md4, online)

- **Unit tests** (`tests/mod_online_tests.cpp`, 3 tests in omega_core_tests):
  - the report codec round trip and its refusals (a bad class, a bad fingerprint, a short body, an over-long
    summary); the classes of each entry kind; the fingerprint's change with a file, the order and the enabled set;
    the three policies;
  - a real `OmegaServer` (transport-free; 132md4's gate, folded into the server by 132on2c): a cosmetic client
    creates under `cosmetic`, a
    gameplay one is refused with `-985`, the echoed MessageID and the notice, a port client with no report is
    refused, an original PS2 session is let through, `none` refuses cosmetic mods, a bad report is ignored;
  - a TCP loopback: the port's `OnlineLobby` sends its report to `omega_server`, which logs it and refuses
    the gameplay-modded create with the notice.
- **The tool:** `omega_mod_tool report` on a cosmetic mod and a gameplay mod (one `files\` script): `gameplay`,
  refused by `cosmetic` and `none`; with the gameplay mod disabled: `cosmetic`, a new fingerprint, refused only by
  `none`; with both disabled: `none`, no fingerprint, allowed by every policy.
- **The server:** `omega_server --mod-policy cosmetic` logs `mod_policy=cosmetic`; `--mod-policy strict` is refused.
- **Neutrality:** with no mods the 8 play regressions are Identical and the restart and level-end chain are
  unchanged.

## LAN: the mod report at join

Checkpoint 132md5. A LAN game has no omega_server, so the joiner sends its mod report to the host inside the join
handshake, and the host decides (`src/port/net/lan_mod_gate.*`; the transport side is in
[LAN_PLAY.md](LAN_PLAY.md) "Checkpoint 132md5").

- **The report.** The same report as online (the class, the fingerprint, the counts and the summary; "Online: the
  mod report" above), asked when the JOIN_REQ is built, so a change in the MODS page counts at the next join. It
  travels as the JOIN_REQ's join extension: `SFMR`, a version byte (1), then the online encoding.
- **The host's policy.** `lan_mod_policy=` in the port settings, or `--lan-mod-policy` for one run:

  | Policy | A joiner may join when |
  |---|---|
  | `gameplay` (default, 132md16) | its gameplay set is the host's (the gameplay fingerprint: gameplay `files\` entries and data fields); cosmetic mods may differ |
  | `any` | always; the host still sees its class |
  | `cosmetic` | its mods change nothing but presentation (class `none` or `cosmetic`) |
  | `none` | it has no enabled mods |
  | `equal` | its fingerprint is the host's own: the same mod files in the same order, or both without mods |

  A joiner that sent no report (an older build, a malformed extension) is refused by every policy but `any` and
  `gameplay` (which counts it as having no gameplay mods; an older build's gameplay-class report cannot be compared
  and is refused).
- **The joiner's notice.** A refused join gets JOIN_NAK `Refused` with the host's reason, and JOINMIS's
  "Failed to join mission" modal shows it instead of F_TO_JOINGAME's text, for example *Refused by the host: Your
  mods change gameplay (1 gameplay entry); this game allows cosmetic mods only.* The other reasons are *You have
  mods enabled (cosmetic, 3 entries); this game allows no mods.*, *Your mods differ from the host's (yours 1a2b3c4d,
  the host's none); this game requires the host's mods.* and *No mod report came with the join; ...*.
- **The host's notice.** The host logs each joiner, accepted or refused: *LAN joiner NAME (address): mods gameplay
  (1 enabled, fingerprint 20e77e5c), refused (policy cosmetic): ...*, and draws the line for about 8 seconds over
  the bottom-left corner of the front end (at most three notices, in the overlay's font). A joiner retries its
  JOIN_REQ every 250 ms; a refusal is logged once per joiner and reason.
- **Online.** The online lobby's games use the same LAN handshake after omega_server's JoinGame, so the host's
  `lan_mod_policy` applies there too, after the server's own `mod_policy`. The online join modal still shows
  F_TO_JOINGAME's text for a refusal (the online code is 132on2c's; see the Handoff).
- **Limits.** As online, the report is self-declared: it keeps honest players with other mods out of a game that
  asks, and a modified client can lie. The fingerprint covers the mods' files, not the ISO (the build hash already
  makes both players run the same port build on the same disc).

## Evidence (132md5, LAN join)

- **Core tests:** `test_lan_mod_gate` (`tests/lan_mod_gate_tests.cpp`) on the loopback network with a manual clock:
  the policy names and the `lan_mod_policy=` setting; the extension's round trip for each class and its refusals
  (empty, another tag or version, a short body, a bad class); each policy's refusals and their texts (cosmetic with
  gameplay mods, none with cosmetic ones, equal with different, missing and extra mods, no report); the gate's log
  (one line per accept, one per joiner and reason over five retries, the host's report asked at each check); two
  transports: a cosmetic joiner accepted with its extension kept on the host, a gameplay one refused with
  JOIN_NAK Refused and the reason and no slot taken, a reconnect clearing the old reason, a joiner with no
  extension (the 34-byte JOIN_REQ) seen with an empty extension, three malformed lengths counted as rejected
  datagrams before the filter; with no filter both kinds of joiner accepted; two `LanSession`s: refused under
  `none` with the reason on the joiner's session, then accepted after the joiner disables its mods. The GUI test
  "Front-end GUI LAN join through JOINMIS" shows the host's reason in the JOIN_NAK modal, and keeps F_TO_CONNECT
  when the link was lost.
- **Hidden front-end runs** (`--frontend-only --hidden --lan-play --lan-loopback --lan-loopback-peer 0`, the
  132ln4 join script, a test mod with one `files\` text entry, so gameplay):
  - `--lan-mod-policy cosmetic`: the loopback peer refuses the join; the modal reads *Refused by the host: Your mods
    change gameplay (1 gameplay entry); this game allows cosmetic mods only.*, the window capture shows the host's
    two-line notice at the bottom left, the front end returns to JOINMIS;
  - `--lan-mod-policy any`: the host logs *mods gameplay (1 enabled, fingerprint 20e77e5c), accepted (policy any)*
    and the join loads TORONTO1 as before.
- **Neutrality:** with no mods the 8 play regressions are Identical and the restart and level-end chain are
  unchanged.

## Evidence (132md5, mod panel)

- **Core tests.** `Mod manager` (`tests/mod_manager_tests.cpp`): `with_manifest_value` (in place, any case, CRLF
  kept, appended, invalid results refused), `move_mod` (every neighbour case, the cascades through equal priorities,
  the ends, invalid manifests), `set_mod_choice` (one key per folder, any case; keys with `=` / `#` refused),
  `session_mods_folder`, and a scratch mods folder's rows, conflict summary, details, a move written to the manifests
  (and refused when a manifest changed meanwhile), `mods=0` and a malformed settings file. `Launcher mod panel`
  (`tests/launcher_mod_panel_tests.cpp`): the `--mod-panel` options and folder choice, then a hidden panel driven by
  its script on a scratch folder (`select`, `toggle`, `up`, `all`, `capture`): the settings file gets `mod.small=0` and
  `mods=0` with its comment kept, `small`'s manifest `priority=6`, the log lines, a 900x620 capture that is not
  blank, `play`, and three script errors.
- **Captures.** `OmegaStrain.exe --mod-panel-script <file> --hidden --mods <scratch>` on four synthetic folders (two
  texture mods overlapping on one member, a gameplay `files\` mod, one without a name): the panel before and after
  `select small`, `up`, `up`, `toggle`, `select tweak`, `toggle` (stage 3 of the 132md5 delivery). The panel draws
  itself into the capture (each control through `WM_PRINT`), so it works for an unshown window. `omega_mod_tool
  list / conflicts / move` on the same folder agree with it.

## Editing in Blender

Checkpoint 132md3. The exports open in Blender (File > Import > glTF 2.0) and its glTF exporter writes a file the
port accepts. `tools/blender_round_trip.py` proves it on the command line for characters, faces, hands, guns and
level models, including the colour-patched and animated ones.

### Steps

1. Export: `omega_mod_tool export-model --out mods\my_mod <what>` (a `.SKL`, a part, a gun archive, a level
   archive; see "Tools"). The new mod's manifest has `enabled=0`.
2. Import into Blender with the defaults: Bone Dir "Blender", Guess Original Bind Pose on, Merge Vertices off.
   The skeleton, the skin, the shape keys (`targetNames`), the vertex colours (`COLOR_0`) and the material names
   (`material_NN`) come in.
3. Edit. Keep the armature (its names, its hierarchy and its rest pose), the shape keys' names and the material
   names; Blender's `.001` suffix on a material name is accepted. Do not apply transforms to the armature.
4. Export glTF 2.0 over the original name (`<member>.SKM.glb` or `.gltf`; remove the other one) with:

   | Option | Setting | Why |
   |---|---|---|
   | Format | glTF Binary (`.glb`) or Separate | either is read |
   | Include > Limit to | the model's armature and mesh | one model per file |
   | Transform > +Y Up | on (the default) | the export's axes |
   | Data > Mesh > Vertex Color | **Active** | the default "Material" writes `COLOR_0` (the lighting, the patches) only when a material uses it |
   | Data > Mesh > Apply Modifiers | off | apply a subdivision in the modifier stack yourself; the armature modifier must stay |
   | Data > Shape Keys | on, with Shape Key Normals | the face and hand shapes |
   | Data > Armature > Skinning | on, **Include All Bone Influences** | the default keeps 4 per vertex, the export may hold more |
   | Animation | off | the game's animations drive the bones |

   The command line's equivalent: `bpy.ops.export_scene.gltf(filepath=..., export_format='GLB',
   export_animations=False, export_all_influences=True, export_vertex_color='ACTIVE', export_morph=True,
   export_morph_normal=True, export_skins=True, export_yup=True, export_apply=False)`.
5. Check: `omega_mod_tool check-models --mods mods` lists each model accepted or refused with the play's reason,
   without starting the game. Then set `enabled=1`.

The port reads what Blender writes: sparse accessors (Blender writes shape keys sparse by default) and accessors
without a buffer view (all zeros). Before 132md3 a Blender face was refused for its sparse targets.

### The round trip

```
python tools\blender_round_trip.py [--blender EXE] [--iso F] [--out DIR] [--keep] [--subdivide N] [what...]
```

- It exports, by default, the regression agent (`TORONTO1 PC_MALE.SKL`, with the face and hands), Gabe
  (`UKRAINE_GABE.SKL`), Lian (`TOKYO LIAN_COMBAT.SKL`), TORONTO1's `WPN.HOG` guns, the level pieces `A07` (colour
  patches), `A26` (an animated material), `A103` and `A123`, and `LGT_REDWARN_GLOW` (an object) into
  `<out>\export\rt_export`.
- It runs `blender --background --factory-startup --python tools\blender_round_trip.py -- --convert <list>`, which
  imports each file with the defaults and exports it with the settings above into `<out>\blender\rt_blender`
  (`.glb`).
- It runs `check-models` on both and compares, per model: accepted in both; the triangles, image sizes, materials,
  morph targets (count and bound shapes), colour patch sets, animated surfaces and skeleton; and the bounds of every
  `POSITION` (within 1e-4 of the extent). Vertex counts are reported (Blender splits vertices at seams).
- It writes `<out>\round_trip.txt` and `blender_log.txt` (default `local\blender_rt`, never committed), and exits 1
  on a difference. Blender is `--blender`, else `%BLENDER%`, else `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe`.
- `--subdivide N` (132md6) makes it an HD edit: after the import every mesh is subdivided N times (bmesh
  `subdivide_edges`, one cut, then triangulated), so each triangle becomes 4^N. The skin weights, shape keys, UVs and
  vertex colours are interpolated by Blender. The disc's normals are authored and Blender recomputes normals after a
  bmesh edit, so the script carries each imported vertex's normal through the subdivision and sets it as the custom
  normal (`normals_split_custom_set_from_vertices`); without that a part's shading changes. The comparison then
  expects 4^N times the triangles, with the same bounds, morph targets and skeleton.
- **One level per mod folder.** A part in `COMMON` (the faces, the hands, `MANSKM` parts) is exported with the
  texture of its name that the exported level supplies, so its key (e.g. `GAMEDATA/COMMON/MORPH.HOG/1HEAD_PLA_DEFA0.SKM`)
  is the same for every level but its image (and the UVs' meaning) is not: TORONTO1's agent head is a 128×64 shaved
  scalp, UKRAINE Gabe's head of the same name a 128×128 one with black hair. Exporting both into one folder keeps the
  later one. The round trip says so (`Note: <key>: <first>'s export replaced by <second>'s` and, in the summary,
  `N export(s) replaced by a later level's`): with TORONTO1's `PC_MALE.SKL` and UKRAINE's `UKRAINE_GABE.SKL` (both in
  its default list) it prints 5 (the eyes, both hands, the head, the neck). A
  replacement of such a key applies in every level, with its own embedded image.

### Limits (Blender)

- **Triangles drawn twice.** A few disc models draw a triangle twice on the same vertices in the same winding (a
  strip artefact; the shuriken clip has 8). Blender keeps one; the image is the same. The round trip counts them and
  allows the difference. A two-sided pair (the same vertices in the other winding) would lose a side, so the round
  trip reports it as a difference; give the back face its own vertices. None of the tested models has one.
- **Vertex colours** come back as 16-bit (`0.5` reads `0.5029`), so a lit surface may move by 1 or 2 levels in
  8 bits.
- **Normals** may differ slightly on a few vertices (9 of 1856 shared ones on `A07`).
- **Skinning.** An edit that adds a bone, renames one or moves the rest pose is refused (the armature must be the
  game's).

## Evidence (132md3, Blender round trip)

- **Blender 5.2.1 LTS on the command line** (`python tools\blender_round_trip.py`): 109 models re-exported in under
  a minute (12 s to 41 s), all 109 accepted and identical in what the game uses: 16495 triangles, 633 morph targets on the faces
  and hands, 10 colour patch sets, 2 animated surfaces; vertices 17580 → 17703 (seams split); the bounds within
  3.4e-07 of the extent; the shuriken clip's 8 triangles drawn twice on the disc kept once. Exit 0.
- **`check-models`** reproduces the play's refusals without playing: `md2_bad` 4 refused, `md2_level_bad` 4,
  `md3_guns_bad` 4 (of 584 parts), `md3_morph_bad` 2; `md3_refused` 28 accepted.
- **Core tests** (`tests/model_tests.cpp`, `test_model_gltf_json`): a sparse accessor over a buffer view and over
  zeros, and the refusal of descending or out-of-range sparse indices.
- **Play** (`run_play_scratch.ps1 -Frames 120 -Extra @('--textured','--capture','120','--mods',...)`): with
  Blender's re-export as the mod, against the tool's own export as the mod, the trace is equal and the frame differs
  in 49075 of 961904 pixels, each by 1 or 2 levels (the 16-bit vertex colours). The mod log lists Blender's files.
- **Neutrality:** with no mods the 8 play regressions are Identical, and the restart and level-end chain are
  unchanged.

## Higher resolution

Checkpoint 132md6. A mod may go past the disc's sizes: images larger than the original texture, and character, gun
and level models with more vertices than the PS2 could hold. What the port takes, and where it stops:

| Kind | What a mod may supply | Limit | Where |
|---|---|---|---|
| Any texture (level, character, gun, interface, font, loading screen) | PNG / DDS of any size, each side 1..8192 | `image_size_limit` 8192; a larger side is refused with a log line (`outside 1..8192`) and the original drawn | `src/port/mods/image_files.hpp` |
| Character parts (`MANSKM`, `MORPH`, `NPCSKM`) and gun parts | any vertex count, 32-bit indices, up to 8 bone influences per vertex, any bone of the skeleton | `replacement_vertex_limit` 1,048,576 vertices, `replacement_influence_limit` 8; more than 1024 morph targets refused | `src/port/gfx/character_model.hpp`, `character_model.cpp` |
| Level models (pieces, sky, objects, NI models, HUD map) | any vertex count | `static_vertex_limit` 1,048,576 vertices, `static_triangle_limit` 2,097,152 triangles | `src/port/gfx/static_model.hpp` |
| Bones | the game's skeleton (names, hierarchy, rest pose) | no new bones: the animations, aiming and hit volumes are the disc's | "Replacement rules" |

- **Textures.** Only the GPU upload changes ("Texture semantics"): UVs are normalised by the original's size, so a 4×
  image covers the same surface with 16× the texels. The scene samples a full mip chain built from the replacement
  (a 4× level texture does not shimmer at a distance); the GUI and the particles sample level 0, so an interface
  image much larger than its drawn size is minified bilinearly. Any ratio works (the sides need not be powers of two,
  nor keep the original's aspect, though a different aspect stretches).
- **Fonts.** A font page at 4× (`FONTDEFA` 256×256 → 1024×1024, `FONTCALL` 256×128 → 1024×512, `FONTLARG`
  512×256 → 2048×1024) draws every glyph in the same place and size with 4× the detail; the glyph rectangles,
  advances and kerning are the `.FNT`'s. Scale the whole page; keep each glyph inside its cell.
- **Characters and guns.** The disc's parts are cut into VU1 packets of at most 21 palette bones each and a few
  hundred vertices; the port's skinning draws a replacement in one piece with 32-bit indices and no palette, so
  neither limit applies. The vertices are skinned on the CPU every frame and uploaded, so the cost grows with the
  vertex count. Since 132md7 the skinning runs across the worker pool straight into the drawn vertices ("Speed"):
  the 4× run below (about 640,000 skinned vertices a frame) plays at about 40 ms a frame, 2–2.5× the no-mod
  frame time, where 132md6 measured twenty times. Since 132md8 a part without morph targets and with at most 128
  bones is skinned on the GPU ("GPU skinning (132md8)"): its vertex count costs only the draw, and the 4× run's
  frame is about the no-mod run's.
- **Level models** are drawn from static buffers (no per-frame cost beyond the draw); the collision stays the
  disc's whatever the drawn mesh.
- **Shared COMMON parts take one image.** See "The round trip": a `COMMON` part's replacement (and its embedded
  image) applies in every level, though the disc's texture of that name differs per level.
- **Not raised:** the screen resolution is the window's (`--window-size`, `window=`), the shadow buffer's size is
  fixed (CHARACTER_SHADOWS.md), the particles are the disc's effects with replaceable images only, and the FMV movies
  are out of scope.

## Speed

Checkpoint 132md7 (port-only). 132md6's 4× run took 856 s for 300 frames against 42 s without mods: HD character
and gun parts were unplayable. The cost was the replacements' CPU path, not the loader: each frame every replacement
part was skinned into a `SkinnedGeometry` (positions, normals, UVs per vertex), copied into the overlay mesh by
`character_mesh` on one thread, copied again into the render thread's snapshot and uploaded; the HUD's gun preview
and the front end's figure skinned theirs a second time. What changed (the no-mod path untouched: it takes none of
these branches, and its traces and the 8 regressions are unchanged):

- **Fused, parallel skinning** (`src/port/render/draw_prepare.hpp` `PartSource`, `replacement_source`;
  `src/port/gfx/character_model.hpp` `ReplacementDraw`, `skin_replacement_draw_into`; `src/port/mods/model_overrides.hpp`
  `replacement_draws`, `gun_replacement_draw`). A replacement part is no longer skinned ahead: `character_mesh` gets a
  source that skins vertex ranges straight into the overlay's `MeshVertex`es (`skin_replacement_vertex`: the
  palette, up to 8 influences, the morph offsets), and splits every part into chunks of 16,384 vertices across the
  worker pool (`worker_threads=`, THREADING.md "Step 5"). The vertex and index arrays are sized once per call (a
  mesh kept from the last frame is only truncated), so nothing is value-initialised twice. The gun's replacement
  parts take the same path (`append_gun_part`). Each chunk writes its own range and the bounds are merged in part
  order, so the result is the serial one byte for byte (`tests/worker_pool_tests.cpp`).
- **The split player.** When the player carries replacements, its piece is built on the main thread with every
  worker on its chunks, and the NPCs' pieces after it as before (`actor_task`, play_main); the pieces are appended
  in actor order (`append_actor_piece`, parallel per piece above 16,384 vertices).
- **The snapshot copy** (`src/port/gfx/gfx_render_thread.cpp`): the render thread's byte buffers come from a pool
  reused frame to frame, and a vertex write above 4 MiB is copied across the workers (`bulk_copy`), where the 4×
  overlay (about 37 MB a frame) was copied on one thread into a fresh allocation.
- **Previews skinned once** (`GunPreviewCache`, `gun_preview_vertices`; the front end's figure in `frontend_disc`):
  the HUD's weapon preview and the loadout figure keep their last vertices and re-skin only when the pose, the
  weapon or the flare changes.
- **Loading.** The mod's PNG / DDS / glTF files are read in one call (were byte by byte through a stream iterator),
  and an archive's path-keyed texture replacements are decoded ahead across threads when the archive is first read
  (`register_texture_paths`: PNG decode and the mip chain), so the per-texture attach on the main thread only takes
  the ready image. The log lines, their order and the images are the same.

**Measured** (TORONTO1, 300 frames, `--textured --gui-overlay --combat`, 1560×1170, `--profile`; 132md6's 4× mod
with the subdivided Blender agent on top: about 640,000 skinned vertices a frame; the shared 20-thread PC loaded to
86–100 % by other agents' builds and runs, so only a pair run back to back compares):

| Run | Wall, no mods | Wall, 4× + subdivided | Frame mean, no mods | Frame mean, 4× |
|---|---|---|---|---|
| 132md6 | 42 s | 856 s | | |
| 132md7 start (same tree) | | 596 s | | |
| pair 1 | 42.1 s | 118.7 s | 26.7 ms | 65.1 ms |
| pair 2 | 29.9 s | 54.3 s (1.8×) | 20.4 ms (sim 11.6, render 8.7) | 40.6 ms (2.0×; sim 11.7, render 28.7) |
| pair 3 (decode ahead) | 114.6 s | 111.5 s | 60.1 ms | 89.9 ms (1.5×) |
| pair 4 | 25.4 s | 39.0 s (1.54×) | 21.3 ms (sim 12.3, render 8.9) | 40.0 ms (1.9×; sim 10.8, render 28.8) |
| pair 5 (as delivered, 74–77 % load) | 19.3 s | 32.5 s (1.68×) | 16.1 ms (sim 9.1, render 6.9) | 39.5 ms (2.45×; sim 10.8, render 28.3) |

So the 4× run went from 20× the no-mod wall time to 1.5–1.8× (pairs 2, 4, 5), and per frame to 2–2.5×: the 1.5×
target is met in wall time, not per frame. The HD frame is about 40 ms (25 fps) whatever the load, with the
replacements' RENDER at about 28 ms against 7–9 ms without mods. The
simulation is unchanged; the rest is RENDER: about 20 ms a frame at 4× for 640,000 vertices.

**Limits of the CPU path.** Growing a `std::vector<MeshVertex>` value-initialises each new vertex on one thread; the
overlay is sized once per `character_mesh` call, but the gun's parts (82,947 to 129,612 vertices each) and the NPCs'
pieces are appended to a mesh already built, which grows it (about 13 % of an HD frame in the sampler). The skinning
itself (`skin_replacement_vertex`) is about 11 %, and the rest is the copy to the render thread and the upload of
about 37 MB a frame. A part of tens of thousands of vertices costs little; a mod of hundreds of thousands per part
is playable at about half the frame rate. Since 132md8 this is the path of `--mods-cpu-skinning` and of the parts
the GPU cannot take (below).

### GPU skinning (132md8)

Checkpoint 132md8 (port-only). A replacement part without morph targets, with triangles and with at most 128
palette bones (`gfx::gpu_skinnable`; `gpu_palette_limit`) is skinned in the vertex program: its bind vertices
(`GpuSkinVertex`) and triangles go to the GPU once per model, and each frame uploads only its palette (12 KB of
constants per draw) where 132md7 skinned, copied and uploaded every vertex. The renderer's side (two programs, one
vertex layout, the Skin constants; kept general for a GXM backend) is RENDERER.md "GPU skinning".

- **Which parts.** On the 4× run below 16 parts are skinned on the GPU (the replaced body parts and the gun's parts)
  and 7 stay on the CPU: the face and hand parts, which morph (expressions and grips: the offsets change per frame),
  each a few hundred vertices. A gun's flare part stays on the CPU (its colour is set per vertex). The log names the
  split when it changes: `Mods: replacement parts skinned (port-only, 132md8, MODDING.md "Speed"): 16 on the GPU, 7 on the CPU`.
  `--mods-cpu-skinning` (diagnostic) keeps every part on the 132md7 path.
- **A device without GPU skinning (132md9).** Whether a part may go to the GPU is asked of the display device
  (`gfx::Device::capabilities().gpu_skinning`, RENDERER.md "Capabilities"): the D3D11 backend says yes; a backend
  that does not implement the two skinned programs (a GXM backend's first cut, a test stub) says no, the default,
  and every part is skinned on the CPU as with `--mods-cpu-skinning`, with no flag and nothing else to set. The log
  says so once, at the level's load: `Mods: the display device (<name>) has no GPU skinning; replacement parts are
  skinned on the CPU`. The question goes through the render thread's queue to the real display (`QueuedDevice`
  forwards it). Runs without the front end build the first frame's actor mesh before the window (and so the display
  device) exists; that one frame is CPU-skinned (its split logged as such), the next ones take the device's answer.
- **How it reaches the draw.** `character_mesh` takes a `PartSource` with `skin` k: it adds the part's material and
  a surface naming `OverlaySkin` k - 1 (its model, palette, colour and thermal rows; `scene_renderer.hpp`) and no
  vertices. The scene pass and the shadow buffer pass draw that surface from the model's buffers with the skinned
  program; everything else in the overlay is drawn as before.
- **The CPU readers of the overlay**, which 132md7 named as the blocker, take values from the bones for a GPU part
  only:
  - *Bounds.* A part's box is the union of its bones' bind boxes (`ReplacementModel::bone_boxes`, made at load:
    the box of the vertices each bone moves) moved by the bone's matrix (`replacement_bounds`: the 8 corners). It
    holds every skinned vertex (a vertex is a weighted mean of its bones' transforms), and is merged into the piece's
    and the overlay's bounds as the vertices' were, so whatever reads them (the actor pieces' bounds, the draw
    list's depth keys) sees a box at least as large.
  - *Thermal.* The vertex program computes the ST from the skinned normal with the same light rows
    (`skm_thermal_uv`) and draws white; the CPU path rewrites the vertices as before.
  - *Ghosts* (the replays' and the co-op partner's night trail). A GPU part's history is its model and its bones'
    matrices in the frame's camera space (`ghost_camera_matrix`); a ghost is the part drawn with those matrices
    taken back to the world through the current camera (`ghost_world_matrix`), the same transform the CPU path
    applies to its vertices.
- **Byte-for-byte?** No: the GPU sums the influences in its own float order, so a frame differs from the CPU path's
  by at most 1 per channel. The simulation never reads the overlay, so the trace is the no-mod run's.
- **Load.** The two skinned programs are compiled with the level when a mod replaces models (`prepare_skinning`);
  compiled at the first skinned draw they stalled that frame about 1.3 s.
- **No mod:** no part has a replacement, so no `OverlaySkin` is made and no skinned program is compiled; the 8
  regressions are Identical and the traces equal.

The overlay pre-size the 132md7 handoff proposed (the gun's and the NPCs' appends growing the mesh) is not needed
on this path: a GPU-skinned part adds no overlay vertices, and the parts left on the CPU are a few hundred vertices
each, as the disc's. It stays a cost of `--mods-cpu-skinning` only.

### Texture decode per level (132md8)

132md7 decoded every path-keyed replacement of an archive across threads when the archive was first read, used or
not. Now (`texture_replacement.cpp`):

- `register_texture_paths` only maps the archive's members to their mod images; nothing is decoded.
- An image is decoded when a decode first references it (`attach_replacement`, the decode hook), outside the lock:
  another thread wanting the same image waits for it; a failed decode logs once and draws the original.
- The level's texture binding (00377570's entries, play_main) decodes its originals first, hands them to
  `decode_replacements_ahead`, which decodes the replacement images they reference (and only those) across
  threads, then attaches them in entry order (`attach_replacements`), so the attached images, the log lines and
  their order are those of the serial hook (the 4× run's 922 mod lines compare equal).
- The PNG inflate is faster (a 64-bit bit buffer, a 10-bit Huffman lookup, in-place match copies, stored blocks
  copied whole) and the PNG unfilter runs per filter type, with a direct RGBA8 path (`image_files.cpp`).

A level that references a small part of a texture pack now decodes only that part; on TORONTO1 the 4× pack's
referenced set is nearly the whole pack, so the load is about 132md7's.

**Measured (132md8)** (the same run as above: TORONTO1, 300 frames, `--textured --gui-overlay --combat`,
1560×1170, the 4× pack and the subdivided agent; the PC shared with other agents' play runs and builds at 60–100 %
load, so pairs run back to back and only their ratio compares; the frame mean is over every frame):

| Pair | Wall no-mods | Wall 4× (GPU) | Wall ratio | Frame no-mods | Frame 4× (GPU) | Frame ratio | Frame 4× `--mods-cpu-skinning` |
|---|---|---|---|---|---|---|---|
| t1 (serial lazy decode) | — | — | 1.6–1.9× | 19.02 ms | 20.50 ms | 1.08× | 38.81 ms |
| t2 (serial lazy decode) | — | — | 1.6–1.9× | 19.81 ms | 16.45 ms | 0.83× | 37.79 ms |
| u1 | 30.6 s | 26.2 s | 0.86× | 16.06 ms | 25.56 ms | 1.59× | — |
| v1 (machine stalled) | 31.2 s | 267.4 s | — | 14.72 ms | 50.75 ms | — | 102.64 ms |
| v2 | 22.0 s | 95.2 s | 4.33× | 27.48 ms | 40.48 ms | 1.47× | 39.54 ms |
| w1 | 16.9 s | 23.6 s | 1.40× | 15.21 ms | 18.65 ms | 1.23× | — |
| w2 | 16.1 s | 18.6 s | 1.16× | 16.12 ms | 16.16 ms | 1.00× | — |
| w3 | 21.8 s | 37.0 s | 1.70× | 14.32 ms | 17.87 ms | 1.25× | — |
| x1 | 17.7 s | 17.8 s | 1.01× | 14.79 ms | 12.43 ms | 0.84× | 35.04 ms |
| x2 (100 % load) | 19.7 s | 43.2 s | 2.19× | 13.30 ms | 21.33 ms | 1.60× | 44.05 ms |
| y1 (rebased) | 20.5 s | 27.1 s | 1.32× | 16.66 ms | 19.34 ms | 1.16× | — |

t1/t2 ran before the batched decode (their wall ratio is from the same runs' timings); the rest run the delivered
build. Every run's trace is the no-mod run's (16ED597F…); every 4× run binds 667 `.TDX` replacements and skins 16
parts on the GPU and 7 on the CPU (`--mods-cpu-skinning`: 0 and 23). Leaving out v1 (the PC stalled: 50 ms frames
and 4–5 minute runs for every variant), the post-batch frame ratio has a median of 1.24× (0.84–1.60×; the sums
give 1.28×), against 132md7's CPU skinning at 2.3–3.3× in the same pairs; the wall ratio's median is 1.36×,
noisier (it includes decoding the pack's 667 referenced PNGs on a PC whose cores are mostly taken).

**Why the GPU path is faster.** 132md7's HD frame spent about 28 ms in RENDER for 640,000 skinned vertices (the
skinning, the 37 MB copy to the render thread and its upload); with GPU skinning those vertices stay on the GPU and
RENDER is the no-mod run's plus the skinned draws.

### Texture memory (132md9)

A low-memory platform cannot hold an HD pack at full size: the 4× pack's TORONTO1 images take several hundred MB as
uploaded, and the PlayStation Vita has 512 MB of RAM and 128 MB of CDRAM in all. The texture memory cap
(`mods_texture_cap=<MB>`, `--mods-texture-cap <MB>`; `mods::set_texture_memory_cap`) bounds what a level's
replacement images take:

- **Off on the PC.** The default is 0 (no cap) on the PC: nothing below happens, the images are decoded at their
  full size as in 132md8. A Vita build's default is 64 MB (`default_texture_memory_cap_mb`: half the CDRAM).
- **Per level, one side limit.** At the level's texture binding (`decode_replacements_ahead`), before decoding, the
  images the level references are measured from their headers (`read_image_size`: no decode) and counted as uploaded
  (`replacement_bytes`: RGBA8 with the full mip chain). If they fit, nothing changes. Otherwise the largest power of
  two side limit L whose total fits is chosen (`plan_texture_cap`): every image wider or taller than L is halved with
  the box filter of the mips until it is not, but never below its original's size (an HD image never ends smaller
  than the disc's). If even the originals' sizes do not fit, every image is at its original's size (the floor) and
  the log says so.
- **Outside a level's batch: a running total.** An image decoded when something first references it (the front
  end's screens, which have no level batch, or a texture first referenced in play) takes the last plan's limit (none
  before the first level), halved further while the cached images and it exceed the cap; the cached images nothing
  holds any more are dropped first (`lazy_limit`). So the front end's screens fit one at a time, the first image of
  a screen at the largest size there is room for.
- **The log.** One line per level with a cap: `Mods (port-only): texture memory cap 64.0 MB: the level's 650
  replacement images take 259.2 MB, scaled to at most 128 px a side: 52.9 MB` (TORONTO1 with the 4× pack), and each
  scaled image's replacement line ends `, scaled from 256x256 by the memory cap`.
- **Across levels.** A cached image whose size differs from the new level's plan is decoded again at the new size
  (one still drawn by a previous holder stays alive until it is released); images the new level does not reference
  are dropped from the cache.
- **Model images (132md10).** The images inside glTF models (a replaced character part's, gun's or level model's material,
  and the front-end figures') are made through `capped_replacement_texture`: they take the running total above (the last
  plan's limit, halved while the cached images, the model images still held and it exceed the cap), never below the
  size of the part's original texture. Each scaled one is logged once: `model image 256x256 drawn at 128x128 (scaled
  by the memory cap)`.
- **Files replaced in `files\` (132md10).** A replaced TDX, or a replaced archive (and the archives nested in it),
  is the game's own format: when the layer resolves it under a cap it decodes the TDX members whose bytes differ from
  the disc's (`register_file_textures`) and records each frame that differs from the disc's frame, with the disc
  frame's size as its floor (a member the disc does not have keeps its size). Those frames count in the level's plan
  when the level binds them and in the running total otherwise (one drawn at its own size counts until the next
  level's plan), and one past the limit is halved for the GPU: the `Texture` keeps its pixels and size (every CPU
  reader sees the file's texture), only the uploaded copy is smaller (`files/GAMEDATA/COMMON/MANTEX.HOG/
  1HAIR_A08_BLAC0.TDX: 128x64 drawn at 64x32 (scaled by the memory cap)`). Its raw alpha (the `As > 1.0` values) is
  halved with it.
- **Without a cap** (the PC's default) none of this runs: model images are made as in 132md9 and files\ textures
  are not decoded twice or recorded.

The simulation never reads the replacement images, so a capped run's trace is the uncapped run's; the frames differ
(smaller images). `omega_mod_tool validate` ("Validating a mod") shows each level's result under the Vita's cap
before a mod is shipped.

### Frame cost (132md11)

After 132md10 the 4× pack's frame had drifted back above 1.5× the no-mod frame in some runs. The profile
(`--profile`, its `render_thread.csv`, and runs of the pack's textures alone and its models alone,
`local\test_mods\hd_tex` / `hd_mdl`) put the cost on the render thread, not on the textures:

- **Textures cost nothing per frame.** The textures-only runs' frame medians (14.06 / 12.21 ms) equalled the no-mod
  runs' beside them (14.30 / 12.64 ms). The override map is consulted when a texture is decoded or attached (load
  time, `attach_replacement`), never per frame; the mips are built once at decode; a replacement binds one view where
  the original bound one, through the same samplers (no state or sampler change per texture). Nothing to fix there.
- **The overlay's buffers (the fix).** A run's first overlay mesh is built before the window's device is known, so it
  skins every replacement part on the CPU, which grew the overlay's two dynamic buffers to about 58 MB and 22 MB. The
  buffers only ever grew, so every later frame's whole-buffer `WRITE_DISCARD` renamed 80 MB on the driver: the render
  thread's execute time was 4.6–18.5 ms a frame with the pack against 1.5–1.7 ms without. `rewrite_buffer`
  (`scene_renderer.cpp`) now makes a buffer again when it is more than four times what the frame needs and over
  1 MiB (still 1.5× headroom when it grows), so after the first frame the buffers are the GPU-skinning frame's size;
  the execute time is back to 1.6–2.0 ms. Pixel-neutral (`tests/gfx_skinning_capability_tests.cpp`
  `test_scene_overlay_buffer_resize`, a device stub counting the re-makes).
- **Skin constants once per skin.** The palette constants of a skin are uploaded once a frame into their own buffer
  and bound by every later draw of it (the shadow pass, the main pass, the man draw's two halves), where each draw
  uploaded them again.
- **Automatic levels of detail.** A GPU-skinned replacement part without morph targets and with at least 4096
  triangles (`replacement_lod_triangles`; the disc's parts have at most about 2000) gets coarser levels at load
  (`src/port/gfx/mesh_simplify.cpp`: quadric error metrics by half-edge collapse onto existing vertices, so every
  level indexes the part's own vertex buffer; seams, borders and non-manifold corners are kept, a collapse that flips
  or slivers a triangle is refused; each level about a quarter of the last, down to 512 triangles). The parts' levels
  are built on threads (one per part, then joined), logged: `models/GAMEDATA/COMMON/MANSKM.HOG/1VEST_V02_V02-0.SKM: 4
  levels of detail (113152/28288/7072/2466 triangles, largest error 0.359, 1735 ms)`. The draw picks, per skin and
  pass, the coarsest level whose error is under half a pixel at the part's nearest point on screen
  (`replacement_lod`, `SceneRenderer::skin_range`); the CPU path and parts with morphs draw whole. `--mods-lod off`
  draws every part whole. On the 4× run five parts get levels (the vest and four Beretta parts: 48,639–113,152
  triangles each); at play distance they draw at their second or third level.

**Measured** (TORONTO1, 300 frames, the md8 command, `local\md11_run.ps1`; frame = `--profile`'s per-frame time past
the first 10 frames; the PC shared with other agents' builds and play runs at 95–100 % CPU throughout, so it was
never idle and medians are the figure to read):

| Round | No mods: median / mean | 4× pack: median / mean | 4× `--mods-lod off` | 4× render-thread execute (median) |
|---|---|---|---|---|
| before the fix, 1 | 12.87 / 13.77 ms | 16.40 / 17.75 ms | | 4.63 ms |
| before the fix, 2 | 11.18 / 11.45 | 18.00 / 18.45 | | 16.49 |
| before the fix, 3 | 11.13 / 11.27 | 22.55 / 23.45 (2.0×) | | 18.45 |
| before the fix, 4 | 14.34 / 17.45 | 20.48 / 23.28 | | 6.10 |
| fixed, 1 | 15.25 / 18.66 | 13.32 / 15.49 | 12.28 / 18.42 | 1.96 |
| fixed, 2 | 14.90 / 17.02 | 13.41 / 20.92 | 14.97 / 24.39 | 2.04 |
| fixed, 3 (the PC stalled for all three) | 14.31 / 20.48 | 32.12 / 28.79 | 35.17 / 30.98 | 1.59 |
| fixed, 4 | 14.79 / 20.05 | 15.16 / 19.14 | 13.22 / 21.04 | 1.88 |

With the fix the 4× pack's median frame is the no-mod run's (0.87–1.03× in rounds 1, 2 and 4; round 3's stall hit
both pack runs) and the means are within 1.0–1.23×, under the 1.5× target; before it the median was 1.3–2.0×.
The delivery's interleaved rounds are in "Evidence (132md11)".

## Mods on the Vita (132md11)

The PlayStation Vita's port (a GXM backend behind `gfx::Device`, in progress) caps a level's replacement images at
64 MB ("Texture memory"). As RGBA8, the 4× pack's TORONTO1 images take 302 MB, so the cap scales them to at most
128 px a side. A pack converted for the Vita fits at full size instead:

```
omega_mod_tool pack --platform vita [--max-size 1024] [--out <file.zip>] <mod folder>
```

- **What it does.** Every `textures\` image (PNG or DDS, `by-hash` too) is converted
  (`mods::platform_texture`, across threads) and stored as `<name>.dds` in the archive; everything else is
  packed as it is. Each side becomes the largest power of two at or below it (GXM's swizzled textures are
  power-of-two; at least 4), halved while either side exceeds `--max-size` (default 1024; 4..4096), resampled by area
  averaging; the full mip chain is built with the box filter the game uses; an image whose every texel is opaque
  becomes BC1 (DXT1, 4 bits a texel), any other BC3 (DXT5, 8 bits a texel, the alpha kept exactly at 0 and 255 where
  a block has both). A DDS already in that form (BC1 / BC3, power-of-two, within the limit, the whole chain) is kept
  byte for byte. A folder holding both `X.png` and `X.dds` is refused. The output is deterministic. Since 132md12 a manifest
  `overrides=` pattern naming a texture's file (`textures/...png`) also matches the packed `.dds` (the stem rule of
  "The manifest"); before it, only patterns on the texture's key (without the extension) matched both.
- **Why BC1 / BC3.** GXM samples them directly (`SCE_GXM_TEXTURE_FORMAT_UBC1` / `UBC3`), vitaGL takes them as S3TC
  DXT1 / DXT5, and Direct3D 11 as BC1 / BC3, so one archive works everywhere: on the PC the same textures load as
  blocks too. PVRTC needs square power-of-two images and an encoder far slower than this one for similar quality,
  and 4 / 8-bit palettes lose the gradients of HD textures, so neither is written.
- **The manifest.** The archive's `manifest.txt` gets `platform=vita` and, unless the mod has one, a `notes.vita=`
  line describing the conversion (`packed by omega_mod_tool pack --platform vita: 1012 textures as BC1/BC3
  (DXT1/DXT5) with mips, power-of-two, at most 1024 px a side (176.5 MB)`). The launcher's details show "Packed for
  vita (loads here too)" on the PC; `list` shows `platform: vita`. The folder is not changed.
- **The tool's report:** `vita: 1012 texture(s) converted (599 BC1, 413 BC3; 36 resized; largest side 1024 px):
  1373.8 MB as RGBA8 with mips -> 176.5 MB of blocks`.
- **In the game** (`texture_replacement.cpp` `decode_one`, `src/port/gfx/texture_blocks.cpp`). A BC1 / BC3 DDS with
  its whole mip chain is not decoded: the `Texture` keeps its blocks (`block_bytes`, `blocks`) and counts as blocks
  in the memory cap (`replacement_bytes(w, h, block_bytes)`: a quarter / an eighth of RGBA8). A cap that wants it
  smaller drops its top levels (no re-encode). The renderer uploads the blocks as `gfx::Format::bc1` / `bc3` on a
  device whose `Capabilities::block_textures` is set (the D3D11 backend sets it; a GXM / vitaGL backend sets it when
  it maps the two formats) and the base level's sides are multiples of 4; on any other device the blocks are decoded
  to RGBA8 at upload (`gfx::upload_levels`), so a pack works on every backend. A texture whose original has raw alpha
  (the `As > 1.0` values) is decoded as before, since its alpha levels are RGBA. A DDS without its whole chain is
  decoded like any image.
- **Model images** (a glTF's own PNGs) are not converted; the runtime cap scales them ("Texture memory").
- **validate** counts a BC DDS as blocks: the Vita pack of the 4× mod shows `TORONTO1 661 40.6 MB full size:
  40.6 MB` (32 % of the CDRAM) where the PNG pack shows `302.3 MB at most 128 px: 56.0 MB`.
- **Quality.** BC1 / BC3 are lossy (565 end points, four colours a 4×4 block); on the 4× run the Vita pack's frames
  differ from the PNG pack's by a mean of 0.03–0.17 per channel (largest 46 on a few texels of high-contrast
  detail).

## Wide backgrounds (132md12)

Port-only. The game draws its menus and loading screens at 4:3; on a wider window the port puts them in a 4:3
rectangle (the frame fit) and fills the sides. A mod can supply the whole wide picture instead:

```
wide.loading=wide/loading_16x9.png,wide/loading_21x9.png      every loading screen
wide.loading.toronto1=wide/toronto_16x9.png                   TORONTO1's loading screen only
wide.frontend=wide/menu_16x9.png                              every front-end screen
wide.frontend.titlescr=wide/title_16x9.png                    one front-end screen (the title)
```

- **Screens.** `loading` and `loading.<level>` (the level's folder name, `toronto1`), `frontend` and
  `frontend.<screen>` (the front end's view name without its `_PANEL<n>` suffix, as in the `--frontend-capture`
  file names: `titlescr`, `onoffsel`, `cmdcentr`). Keys are compared lower case;
  the exact key wins, then the part before the first `.` (`loading.toronto1`, then `loading`).
- **Images.** PNG or DDS (read by content), any size, paths inside the mod's folder (`\` or `/`; no `..`, no drive).
  Several images per key are alternatives: the one whose aspect is nearest the window's (by ratio) is drawn, the
  wider on a tie; an image not wider than 4:3 is never drawn (`validate` warns). The picked image is scaled to the
  window's height and centred: a 16:9 image fills a 16:9 window exactly, leaves black bars on a wider one (supply a
  21:9 image too) and is cut at the sides on a narrower one (16:10). Drawn opaque; its alpha is ignored.
- **Drawing.** `GuiRenderer::draw_wide_background(screen, target, w, h)` (`gui_renderer.cpp`) draws the image over
  the whole target before the 4:3 screen is drawn on top, so the screen's own art, text and replacements are
  unchanged. It returns false, and the port's own fill is drawn, with the frame fit off, a window not wider than 4:3,
  no mods, no image for the screen, or a lost device. The images are decoded once per screen and mod set
  (`mods::wide_background_images`, `texture_replacement.cpp`) under the texture memory cap ("Texture memory": a
  wide image is capped as if its original were 640x448).
- **Where it is called.** The wide (16:9) presentation is 132ws's (research/FRONTEND.md "Wide backgrounds from
  mods"); until it lands the hook exists and is tested but the game does not call it, so nothing changes.
- **Overrides in wide mode.** HUD and menu images replaced by `textures\` entries keep working at any aspect: every
  GUI image goes through the renderer's one upload path (`upload_image`, then `gfx::upload_levels`), and the wide
  layout moves and scales the quads, not the images; UVs stay normalised to the original's size.
- **Several mods.** The highest-priority mod's key wins whole (its images, not a mix); `list` shows
  `wide.loading <- hd_pack: 1 image(s)`, and `conflicts` reports the shadowed mods
  (`shadowed wide.loading ...`).
- **The mod tool.** `list` shows each key; `validate` decodes every image and prints its size
  (`wide.loading wide/loading_16x9.png <- hd_pack: 384x216`), warns on one not wider than 4:3 or over 4096 px a side
  (the Vita's limit), and fails on an unreadable one or one over 16384; `pack` keeps the images;
  `pack --platform vita` keeps them as PNGs at their aspect and scales any wider than 960 px (the Vita's screen) to
  960 px wide (a 1920x1080 image becomes 960x540).
- **The log:** `wide.loading hd_pack/wide/loading_16x9.png: 384x216`, or `: not used (<reason>)`.

## Validating a mod

`omega_mod_tool validate [--iso <file>] [--mods <folder>] [--settings <file>] [--match <text>] [--skeleton <SKEL>]
[--strict] [<mod folder>...]` (132md9) checks everything a mod folder holds against the disc in one pass, with the
play's own code (the scan, the model loader, the sound fitting), and prints what play would do. Named folders are
checked alone (only they enabled); without names, the enabled mods as `local\port_settings.txt` has them. It exits 1
on an error (with `--strict`, on a warning too). Each finding is one line:

| Level | What |
| --- | --- |
| `ERROR` | a mod skipped (bad manifest), no mods folder; a `data_overrides=` file with an unknown name or a value out of range (132md16); an image that is not a readable PNG / DDS or wider than 16384 (the PC's limit); a model or sound play refuses (with play's reason) |
| `warning` | unused overrides: a texture, model, sound or file the disc does not have ("replaces nothing"), a data field naming no weapon record or NPC (132md16), an `overrides=` item that matches nothing, an entry outside `overrides=` ("ignored"), one shadowed by a higher-priority mod; an image not the original's shape or smaller than it; a side over 4096 (the Vita's GXM limit); a level that does not fit the Vita's cap even at the originals' sizes |
| `note` | a model part skinned on the CPU (morph targets, or past the GPU's bone limits; "GPU skinning"); a level scaled on the Vita |

The texture memory table counts each level's images as uploaded (RGBA8 with the mip chain, `replacement_bytes`):
the level's own (`GAMEDATA/<level>/...`) plus `COMMON`'s and every by-hash image (they can bind in any level). The
columns are the PC (no cap: the full size), the Vita under its default 64 MB cap (the plan `plan_texture_cap` makes:
"Texture memory") and that result as a share of the Vita's 128 MB CDRAM and 512 MB RAM (conservative: the game, the
originals and the frame buffers share both). The front end has no level batch (its screens' images are decoded as
they open): its row is the largest screen's. The glTF images inside models are listed apart (under a cap they take
the running total, 132md10). A packed mod (`.zip`) is validated in place, with the same output as its folder.

The 4× test pack (`md6_hd` and `rt_blender`, 1035 entries; about 43 s):

```
Mods folder ...\test_mods\hd: 2 enabled mod(s)
  mod md6_hd "Higher-than-original resolution test (132md6)" 1.0 priority 10: 1017 file(s)
  warning: unused (shadowed): models/GAMEDATA/COMMON/MANSKM.HOG/1VEST_V02_V02-0.SKM from md6_hd (priority 10) shadows rt_blender (priority 10)
  mod rt_blender "rt_blender" 0.1 priority 10: 19 file(s)
Checking 1035 entries against the disc
  models/GAMEDATA/COMMON/MANSKM.HOG/1BELT1HOL_T01-0.SKM <- rt_blender: skinned on the GPU
  ...
  note: models/GAMEDATA/COMMON/MORPH.HOG/1HEAD_PLA_DEFA0.SKM <- rt_blender: skinned on the CPU (morph targets)
  ...
Checked 1012 texture image(s), 0 by-hash image(s) (matched by content at run time; not checked against the disc), 23 model(s) (16 skinned on the GPU, 7 on the CPU), 0 sound(s), 0 file(s)
Texture memory per level (RGBA8 with mips as uploaded; COMMON and by-hash images count in every level):
  level         images PC (no cap)  Vita (64 MB cap)                  CDRAM      RAM
                                                                     128 MB   512 MB
  TORONTO1         661    302.3 MB  at most 128 px: 56.0 MB             44%      11%
  note: TORONTO1's images are scaled on the Vita to at most 128 px a side (302.3 MB at full size)
  FRONTEND*         16     54.7 MB  full size: 54.7 MB                  43%      11%
  * the front end's largest of 48 screen(s): EQSETUP.HOG (the menus decode a screen's images when it opens; the cap there is a running total)
Largest images:
  textures/GAMEDATA/FRONTEND/NTSC.HOG/CMDCENTR.HOG/LINE21.TDX <- md6_hd: 2048x2048, 21.3 MB
  ...
Model images (glTF materials; not in the table: under a texture memory cap they are halved by the running total, never below the part's original texture): 23, 1.0 MB
validate: 0 error(s), 1 warning(s), 8 note(s)
```

A broken mod (a truncated PNG, a member the disc lacks, a sound with no disc file, an `overrides=` item for a level
that does not exist) exits 1:

```
  warning: bad: overrides= GAMEDATA/NOPE matches nothing the folder holds
  warning: textures/GAMEDATA/COMMON/MORPHTEX.HOG/NOPE.TDX <- bad: unused, replaces nothing (the disc's GAMEDATA/COMMON/MORPHTEX.HOG has no member NOPE.TDX)
  ERROR: textures/GAMEDATA/TORONTO1/TEX.HOG/NOTREAL.TDX <- bad: not a readable PNG / DDS (Not a PNG file); the disc's texture is drawn
  ERROR: sounds/GAMEDATA/COMMON/X <- bad: refused (No disc file GAMEDATA/COMMON/X)
validate: 2 error(s), 2 warning(s), 0 note(s)
```

Data overrides (132md16, "Gameplay data overrides") list every value the game will use, the disc's and the mod's,
for each patched record:

```
Data overrides (gameplay: the LAN / online gameplay policies need the host's set):
  WEAPONS.WDB record 112 SocomSilenced damage_mid: 80 -> 150 (stronger_silenced_pistol)
  WEAPONS.WDB record 112 SocomSilenced damage_near: 80 -> 150 (stronger_silenced_pistol)
  WEAPONS.WDB record 112 SocomSilenced damage_far: 80 -> 150 (stronger_silenced_pistol)
  WEAPONS.WDB record 112 SocomSilenced clip: 12 -> 15 (stronger_silenced_pistol)
```

and a broken one says which name or value is wrong and where:

```
  ERROR: broken: ignored data_overrides b.ini (b.ini line 2: clip 0 is out of range (1..999))
  warning: data weapon NO_SUCH_GUN clip <- tuning (t.ini line 2): unused, no record's display key is NO_SUCH_GUN
  warning: data weapon 999 clip <- tuning (t.ini line 4): unused, record 999 is past the 162 records
  warning: data npc TORONTO1 999999 health <- tuning (t.ini line 8): unused, TORONTO1 has no NPC 999999 (the play log's "Hit at frame" lines give ids)
```

Limits: by-hash images are matched by content at run time, so validate cannot tell whether one replaces anything
(it counts them in every level); the table is an estimate of what the cap's plan does, not a measurement of a run.

## Mod author guide

A mod is a folder in `mods\` next to `OmegaStrain.exe` with a `manifest.txt` and the replaced assets in the layout
of "The mods folder". Start from an export of your own disc (never share disc data you have not changed), change
what you want, delete what you did not change (an unchanged export still overrides lower-priority mods), declare
it, check it and enable it. The four examples below use the commands of "Tools"; paths are examples.

### Every mod: the finishing steps

```
omega_mod_tool declare mods\my_mod            writes overrides= from what the folder holds
omega_mod_tool validate my_mod                everything below in one pass, plus unused entries, image sizes,
                                              CPU-skinned parts and texture memory per platform (exit 1 on an error)
omega_mod_tool check-models --mods mods       models: accepted, or refused with the play's reason (exit 1)
omega_mod_tool check-sounds --mods mods       sounds: fitted to the game's stream / bank sample, or refused
omega_mod_tool enable my_mod                  mod.my_mod=1 in local\port_settings.txt (or the MODS page, or the
                                              launcher's mod panel: hold Shift while OmegaStrain.exe starts)
omega_mod_tool list                           what each mod supplies (used: N textures, M models, ...)
omega_mod_tool conflicts --strict             shadowed / layered / ignored entries against the other mods
omega_mod_tool move my_mod up                 load order (the manifests' priority=)
omega_mod_tool report                         the class (cosmetic / gameplay) online and LAN hosts see
```

Edit the manifest's `name=`, `version=`, `author=`, `description=` and `priority=` (higher wins). Start the game
(or `omega_play`): its log ("The log"; `local\omega_strain.log` when started from Explorer) has one `Mods (port-only): ...` line per asset replaced, with the original's and the replacement's sizes, and a reason for anything refused.

### Example 1: an HD face and body (Blender)

1. Export one character of one level: `omega_mod_tool export-model --out mods\hd_agent
   GAMEDATA/TORONTO1/SKL.HOG/PC_MALE.SKL` writes each part (`models\GAMEDATA\COMMON\MANSKM.HOG\<part>.SKM.gltf`,
   `.bin` and `.png`), the face and hands with their shapes (`MORPH.HOG`). Export one level per mod folder ("The
   round trip").
2. Make the images bigger first: paint or upscale `<part>.SKM.png` (4×: a 128×64 head becomes 512×256). The image
   is embedded in the replacement and drawn at its full size.
3. In Blender (the settings of "Editing in Blender"): import a part, then in Edit Mode select all and
   **Edge > Subdivide** (one cut; repeat for more). Blender interpolates the weights, shape keys and UVs. Sculpt or
   model the extra detail without moving, renaming or adding bones. Blender recomputes the normals; the disc's are
   authored, so check the shading (or let the script below keep them). Export over the part's name as `.glb` and
   remove its `.gltf`, `.bin` and `.png`.
4. Or do steps 1 and 3 on the command line: `python tools\blender_round_trip.py --subdivide 1 --out local\hd
   GAMEDATA/TORONTO1/SKL.HOG/PC_MALE.SKL` writes `local\hd\blender\rt_blender` (19 parts, 4× the triangles, the
   shapes and normals kept, checked), a mod folder you can copy into `mods\` and edit further.
5. `omega_mod_tool check-models --mods mods`: each part accepted (`MALE.SKEL, 1182 vertices, 1676 triangles, image
   128x64`), or refused (a renamed or moved bone, a missing shape) with the original drawn. Then the finishing steps.

### Example 2: a level texture pack

1. `python tools\mod_export.py level TORONTO1 --out mods\toronto_hd` (TEX, MAPTEX, NPCTEX, OBJECTS, WPN as PNG,
   with `textures_index.csv`: key, size, frames, hash).
2. Find the textures you see: `omega_mod_tool export-model --out local\pieces --match A123
   GAMEDATA/TORONTO1/DATA.HOG` exports the level pieces whose names contain the text, each material naming its
   texture (`materials[].extras.texture`). Or bisect by colour: replace half of the candidates with one solid
   colour each (8×8 PNGs are enough), look, and halve again; three rounds took TORONTO1's 640 textures to the
   regression room's back wall (`WHITECONC.TDX`).
3. Upscale or repaint the PNGs you want (4× here: `WHITECONC` 64×64 → 256×256); keep the alpha rules of "Texture
   semantics" (255 is opaque; a cut-out keeps its holes). An animated texture has one PNG per frame.
4. Delete the PNGs you did not change, then the finishing steps. The log shows each one:
   `textures/GAMEDATA/TORONTO1/TEX.HOG/WHITECONC.TDX <- toronto_hd (64x64 -> 256x256)`.

### Example 3: a sound pack

1. `python tools\mod_export.py sounds TORONTO1 --out mods\loud_guns --match WEPS` (the voice and music streams
   and the bank samples whose keys contain `WEPS`, as WAV, with `sounds_index.csv` listing which bank sounds use
   each sample: sample 13 of TORONTO1's `WEPS.BNK` is the 92F's shot, among others).
2. Replace `sounds\GAMEDATA\TORONTO1\SND.HOG\WEPS.BNK\13.wav` with your sound (WAV or Ogg Vorbis, any rate, mono or
   stereo; `13.ogg` also works). A bank sample is resampled to the original's rate and cut or padded to its
   length; a stream keeps its own length (see "Sounds and music" and "Sound effects in the banks").
3. Delete the files you did not change; `omega_mod_tool check-sounds --mods mods` prints what each becomes
   (`22050 Hz x1, 0.20 s -> bank sample 13 at 53781 Hz mono; 0.04 s ...`). Then the finishing steps.

Or name the sounds in the manifest instead of placing files by key: `audio_overrides=WEPS.BNK/13=sfx/shot.wav`
replaces sample 13 of every level's `WEPS.BNK`, `audio_overrides=ENVIRON.BNK/O_BANK_ALARM=sfx/alarm.ogg` every
sample the alarm sound plays, and `audio_volume=` sets the mod's level ("Audio overrides"; the example
`tools\mods\examples\audio_pack`).

### Example 4: a UI font

1. `python tools\mod_export.py gui --out mods\hd_font --match FONTDEFA` writes
   `textures\GAMEDATA\COMMON\FONTS.HOG\FONTDEFA.TDX.png` (256×256).
2. Scale the whole page 4× (1024×1024) and redraw the glyphs sharper, each inside its original cell (the `.FNT`'s
   rectangles do not move). Keep the alpha: the glyphs are drawn through it.
3. The finishing steps. The log: `textures/GAMEDATA/COMMON/FONTS.HOG/FONTDEFA.TDX <- hd_font (256x256 ->
   1024x1024)`. `FONTCALL` and `FONTLARG` are the other pages; the loading screens and menu art (`LOADING.HOG`,
   `FRONTEND/NTSC.HOG`) follow the same steps.

### The example mod

`python tools\make_example_mod.py [--out local\example_mods] [--name md6_example] [--priority 5] [--disabled]`
writes a complete mod made only of generated data (no disc bytes; Python's standard library only), a template to
copy:

```
md6_example\
  manifest.txt                                              name=Example pack, version=1.0, author, description,
                                                            priority=5, enabled=1, overrides= the three archives
  textures\GAMEDATA\TORONTO1\TEX.HOG\WHITECONC.TDX.png      256x256 procedural bricks (the disc's is 64x64): the
                                                            walls of the regression view's first room
  textures\GAMEDATA\COMMON\LOADING.HOG\BAR.TDX.png          the loading bar, a 256x256 gradient (the disc's 64x64)
  sounds\GAMEDATA\TORONTO1\SND.HOG\WEPS.BNK\13.wav          a 0.2 s 990 Hz beep with a decay (the 92F's shot)
```

`omega_mod_tool list --mods local\example_mods` reports `used: 1 sounds, 2 textures`, `conflicts --strict` 0
shadowed / layered / ignored, and `check-sounds` the beep accepted. Start the game with `--mods local\example_mods`
(or copy the folder into `mods\`): the room's walls are bricks and the 92F beeps.

`--hd-pack` (132md12) writes the HD texture pack example instead, committed as `tools\mods\examples\hd_pack`
("Making an HD texture pack"); `--audio-pack` (132md13) the audio example, committed as
`tools\mods\examples\audio_pack` ("Audio overrides").

## Making an HD texture pack (132md12)

The whole path, from the disc to a pack on the PC and the Vita. A worked example is in `tools\mods\examples\hd_pack`
(generated by `python tools\make_example_mod.py --hd-pack`; synthetic images, no disc data): a 4× loading bar,
2× bricks on TORONTO1's white concrete and a 16:9 loading-screen background.

1. **Export.** `python tools\mod_export.py level TORONTO1 --out local\export` for a level's textures (with
   `textures_index.csv`), `python tools\mod_export.py gui --out local\export` for the menus, loading screens and
   fonts. Never share images you did not change.
2. **Upscale.** Paint or upscale the PNGs you want, 2× or 4× (any size works; power-of-two sides keep the Vita pack
   unscaled). Keep the alpha rules of "Texture semantics": 255 opaque, cut-outs keep their holes, one PNG per
   animation frame (`<NAME>.TDX.<n>.png`).
3. **Place.** Make `mods\my_pack\` and copy only the changed PNGs into `textures\` with their exported paths
   (`textures\GAMEDATA\TORONTO1\TEX.HOG\WHITECONC.TDX.png`).
4. **Wide art (optional).** Draw 16:9 (or wider) loading and menu backgrounds into `wide\` ("Wide backgrounds").
5. **Manifest.** `manifest.txt`: `name=`, `version=`, `author=`, `description=`, `game_version=SCUS-97264`,
   `priority=` (higher wins), `overrides=` (`omega_mod_tool declare mods\my_pack` writes it; file names with `.png`
   are fine, the stem rule also covers the Vita pack's `.dds`), `wide.loading=` / `wide.frontend=` and
   `notes.pc=` / `notes.vita=`.
6. **Check.** `omega_mod_tool list` (`used: 2 textures`, `wide.loading <- my_pack: 1 image(s)`),
   `omega_mod_tool validate my_pack` (sizes, unused entries and the texture memory per level and platform; exit 0),
   `omega_mod_tool conflicts --strict` (which mod wins each file).
7. **Pack.** `omega_mod_tool pack --out dist\my_pack.zip mods\my_pack` for the PC (the output folder must exist),
   and `omega_mod_tool pack --platform vita --out dist\vita\my_pack.zip mods\my_pack` for the Vita (BC1 / BC3
   with mips, power-of-two, at most 1024 px; "Mods on the Vita"). Each reads its archive back and compares it with
   the folder.
8. **Play.** Put the folder or the zip in `mods\` (or start with `--mods <folder>`) and enable it (the launcher's
   mod panel, the MODS page or `omega_mod_tool enable my_pack`). The log has one line per replaced texture
   (`textures/GAMEDATA/TORONTO1/TEX.HOG/WHITECONC.TDX <- my_pack (64x64 -> 128x128)`).

For the example: `omega_mod_tool list --mods tools\mods\examples` shows `hd_pack ... used: 2 textures` and
`wide.loading <- hd_pack: 1 image(s)`; `validate` 0 errors, 0 warnings (TORONTO1 2 images, 0.4 MB); `conflicts
--strict` 0 / 0 / 0; `pack` a 3573-byte zip; `pack --platform vita` a 4116-byte zip with 2 BC1 textures, the wide
image kept, and `list` over it still `used: 2 textures` under the `.png` `overrides=`.

## Evidence (132md6)

- **The MODS page's key** (`tests/port_settings_page_tests.cpp`, `test_mods_page_key_case`): a settings file holding
  `mod.QUIET=0` and `mod.quiet=0` for the folder `Quiet`; toggling its row on leaves one `mod.Quiet=1` line, the
  older spellings gone and the comments and other keys kept, and the row reads `ON (NEXT)`. Before the fix the page
  added `mod.Quiet=1` beside the old spellings (three keys for one folder, the old ones read as well); the test
  failed then.
- **Core tests** (`tests/model_tests.cpp`, `test_model_high_resolution`): a 300×300 grid (90,000 vertices) on a
  64-bone chain with 8 influences per vertex, loaded and skinned in one piece; a 70,000-vertex gun part through
  `skin_gun_replacement_into`; `replacement_vertex_limit + 1` vertices refused (`more than 1048576 vertices`); an
  8192-wide PNG accepted and an 8193-wide one refused (`outside 1..8192`).
- **4× everywhere** (`python tools\make_test_mods.py --hd`, then the play at frames 100 and 290 with
  `--mods local\test_mods\hd`): 682 replacements logged, among them 650 of TORONTO1's `TEX.HOG` (`WHITECONC` 64×64 →
  256×256), the shirt's `MANTEX` texture 64×64 → 256×256, the three font pages (above), the loading screens and the
  front end's art; the vest 230,779 vertices and 452,608 triangles, the 92F's parts 82,947 to 129,612 vertices, each
  with its image at 4×. `check-models`: 5 accepted. The trace is equal to the run without mods. Each 4× texture
  carries a dark line on every original texel's first row and column, which shows only if the image is sampled at
  its own size: the frames show it on the walls, the agent's shirt and vest, the gun and the HUD's text. The run took 856 s for 300 frames
  against 42 s without mods, almost all of it the CPU skinning of the 640,000 vertices of the five parts: that is
  the practical limit, not the loader's.
- **An HD agent through Blender** (`blender_round_trip.py --subdivide 1`, TORONTO1's `PC_MALE.SKL`, Blender 5.2.1):
  19 parts subdivided in 30 s, all identical in what the game uses: 11,428 triangles (4×), 468 morph targets,
  vertices 2,429 → 7,677, the bounds within 1.8e-07 of the extent. The play draws them with an equal trace (the head
  1,182 vertices and 1,676 triangles with its 128×64 image and 88 shapes), and the head looks as the original's.
- **The dark head.** A first run with two characters (TORONTO1's agent and UKRAINE's Gabe) drew the agent's scalp
  dark: Gabe's export of `1HEAD_PLA_DEFA0.SKM` (and the eyes, hands and neck) had replaced the agent's in the shared
  folder, black hair image included (128×128 against the agent's 128×64). The round trip now notes such keys, and
  this section and "The round trip" say to export one level per mod. Found on the way: Blender's recomputed normals
  differ from the disc's authored ones (a mean dot product of 0.84), now carried through the subdivision (0.93; the
  rest is the new vertices' interpolation).
- **The example mod** (`make_example_mod.py`): `list`, `conflicts --strict` and `check-sounds` exit 0; in the play
  the log shows `WHITECONC.TDX ... (64x64 -> 256x256)` and `WEPS.BNK: 1 of 100 samples replaced`, the room's back and
  side walls are bricks at frame 290, and the trace is equal to the run without mods.
- **Neutrality:** with no mods the 8 play regressions are Identical, the restart revives at frame 212 and the
  level-end chain ends at frame 8735.

## Evidence (132md7)

- **Speed:** the table in "Speed". Every 4× run's trace is equal to the no-mod run's, and its log carries the same
  677 texture replacement lines as before the decode ahead.
- **Core tests:** `tests/worker_pool_tests.cpp` ("Worker pool"): 12 random rounds where the fused sources at 0 and 3
  workers give the same mesh as the skinned geometry through `character_mesh` (empty, larger and smaller stale
  meshes, appends, gore, texture lists, offsets), the parallel `character_mesh` and `append_actor_piece` equal the
  serial ones, a hidden actor, and a failing source rethrown; `tests/model_tests.cpp` (`test_model_high_resolution`):
  the gun draw through `skin_replacement_draw_into` equals `skin_gun_replacement_into`, the preview vertices and
  the cache's hits and misses; `tests/mod_tests.cpp` ("Mod overrides at the disc boundary"): a broken path-keyed
  image decoded ahead still falls back with its log line, and the decoded-ahead image is cached.
- **Neutrality:** with no mods the 8 play regressions are Identical, the restart revives at frame 212 and the
  level-end chain ends at frame 8735.

## Evidence (132md8)

- **Speed:** the table in "GPU skinning (132md8)". Every run's trace equals the no-mod run's (`16ED597F…`); every
  4× run logs the same 677 texture replacement lines as 132md7, and its 922 mod lines compare equal, in order, with
  those of the serial lazy decode (before `decode_replacements_ahead`).
- **Core tests:** `tests/model_tests.cpp` (`test_model_gpu_skinning`): the GPU vertex and palette rows reproduce
  `skin_replacement_vertex` (the shader's sums in C++), `gpu_skinnable`'s limits, `replacement_bounds` holds every
  skinned vertex, and the ghost matrices round-trip; `tests/worker_pool_tests.cpp` ("Worker pool"): GPU-skinned
  sources add their surface and bounds and no vertices, through `character_mesh` and `append_actor_piece`, serial
  and parallel alike; `tests/mod_tests.cpp`: the inflate's fast paths against zlib vectors (`mod_png_vectors.hpp`),
  an image decoded at its first reference (a broken one mended after registration is used), nothing decoded for an
  unreferenced member, and a batch (`WithoutReplacements`, `decode_replacements_ahead`, `attach_replacements`)
  attaching and logging once, at the attach.
- **Neutrality:** with no mods the 8 play regressions are Identical, the restart revives at frame 212 and the
  level-end chain ends at frame 8735.

## Evidence (132md9)

- **Runs** (TORONTO1, 4× pack, the md8 timing command; `local\md9_pair.ps1`): every run's trace equals the no-mod
  run's (`16ED597F…`). The HD run logs 667 texture lines and "16 on the GPU, 7 on the CPU"; with `--mods-cpu-skinning`
  "0 on the GPU, 23 on the CPU"; with `--mods-texture-cap 64` the level's line is `the level's 650 replacement images
  take 259.2 MB, scaled to at most 128 px a side: 52.9 MB` and 455 of the 667 replacement lines end `by the memory
  cap`. Back to back on the loaded machine (wall / frame mean): no mods 28.5 s / 19.6 ms, HD 22.4 s / 22.0 ms, capped
  32.7 s / 19.6 ms; then 24.3 / 17.6, 23.3 / 28.0, 22.5 / 24.8; capped first: capped 35.9 / 26.9, HD 56.2 / 25.3;
  capped 35.1 / 20.3, HD 33.2 / 25.6 (load 91–100%: the means are noise-bound, and whichever run comes later in a
  set is often the slow one; the cap adds no measurable cost).
- **validate:** the outputs in "Validating a mod" (the 4× pack, `rt_blender` alone: 19 models, 12 on the GPU, 7 on
  the CPU, exit 0; a broken mod: exit 1).
- **Core tests:** `tests/gfx_skinning_capability_tests.cpp` ("GPU skinning capability"): a device stub without the
  capability (the default `Device::capabilities()`) gets no skinned programs from `prepare_skinning` and its scene
  renderer reports none (the CPU fallback); a stub that declares it gets both programs; `gfx::gpu_skinning` reads
  false for no device; the render thread's queued device forwards either answer;
  `tests/mod_tests.cpp` (`test_mod_texture_memory_cap`): `replacement_bytes`, `capped_size` (never below the
  original), `plan_texture_cap` (none, under, the largest fitting limit, over at the floor), the setting, PNG and DDS
  header sizes, a capped level batch (scaled, logged, the box filter's colour), a lazy decode at the level's limit,
  a plan with room decoding again at full size while a held image is kept, and the running total outside a batch
  (full size, halved beside a held image, room again after a release). 652 tests.
- **Neutrality:** with no mods the 8 play regressions are Identical, the restart revives at frame 212 and the
  level-end chain ends at frame 8735.

## Evidence (132md10)

- **Packing:** `omega_mod_tool pack` over the 4× pack's folder (1029 files, 80.8 MB) writes `md6_hd.zip` at 50.1 MB
  (832 members deflated, 197 stored where deflate does not shrink them) and reads it back: 1017 entries as in the
  folder. `rt_blender.zip`: 20 files. Python's `zipfile` reads our zip (every member byte-equal), and a zip Python
  writes with a top folder and directory entries is read as a mod (19 models).
- **Runs** (TORONTO1, 4× pack, the md8 timing command; `local\md10_pair.ps1`): the no-mod run, the pack as a folder,
  the pack as a `.zip` and the `.zip` under `--mods-texture-cap 64`. Every trace equals the no-mod run's
  (`16ED597F…`); the folder and the zip each log 667 texture lines and "16 on the GPU, 7 on the CPU", and their mod
  logs (922 lines) differ only in the mod's path. Under the cap the level's line is as in 132md9 (`650 replacement
  images take 259.2 MB, scaled to at most 128 px a side: 52.9 MB`), 457 lines end `by the memory cap` (455 in
  132md9: the two model images now scaled too, `model image 256x256 drawn at 128x128`, `256x128 drawn at 128x64`).
  Wall / frame mean on the loaded machine (57–98% CPU), three sets: no mods 17.4 s / 11.0 ms, folder 19.2 / 22.0, zip
  22.4 / 22.2, capped zip 24.4 / 21.7; then 25.1 / 14.1, 23.7 / 15.3, 24.1 / 21.7, 24.8 / 14.4; after the rebase
  15.0 / 11.0, 22.3 / 21.0, 20.9 / 21.5, 29.0 / 15.2 (noise-bound; the zip costs no frame time over the folder:
  reading in place inflates each member once as it is first read, at load).
- **files\ textures under a cap:** a mod replacing the 1686 small members of `MANTEX.HOG` by distinct larger ones
  (`local\test_mods\files_cap`, cap 1 MB) logs `files/GAMEDATA/COMMON/MANTEX.HOG/1HAIR_A08_BLAC0.TDX: 128x64 drawn at
  64x32 (scaled by the memory cap)`; trace `16ED597F…`.
- **Launcher:** the mod panel with the zip and a folder lists "2 mod(s) (1 .zip)", the row `[md6_hd.zip]`, and the
  details "(archive md6_hd.zip, read in place)", "Load order: 1 of 2", "Contested entries: wins 1, loses 0; 1016
  supplied by this mod alone", the line `wins models/...1VEST_V02_V02-0.SKM over rt_blender` and the set's "Load
  order (the first wins, then the disc): 1. md6_hd, 2. rt_blender".
- **validate:** the zip's output equals the folder's but for the path (exit 0).
- **Core tests:** `tests/mod_archive_tests.cpp`: the zip layer (deflate round trips, deterministic bytes, stored or
  deflated by size, case-insensitive find, prefix reads; refusals: a bad CRC-32, encryption, ZIP64, unknown methods,
  `..`, absolute and case-duplicate names, truncated or non-zip files; member replacement keeping the others' bytes);
  a packed mod read in place as its folder, at the root or under one top folder (the same entries, scan messages and
  fingerprint; a folder beating a same-named zip, a damaged zip skipped, `game_version=` and `notes.<platform>=`); the
  load order, owners, contested entries and details and the priority rewrite inside a zip; the cap over model images
  (halved while another is held, room again once released) and files\ textures (the GPU copy scaled, the pixels and
  raw alpha, the level plan, the running total, a second attach changing nothing, and with the disc image a
  `MORPHTEX.HOG` member replaced by a larger one scaled toward the disc member's size). 657 tests (on origin/main 1867f31e).
- **Neutrality:** with no mods the 8 play regressions are Identical, the restart revives at frame 212 and the
  level-end chain ends at frame 8735.

## Evidence (132md11)

- **Frame cost:** see "Frame cost (132md11)" for the profile and the rounds; the delivery's final interleaved rounds:
  four rounds after the rebase (`z_*`; frame median / period mean, ms; the PC at 100 % CPU with 14 other play
processes, so wall times of 19–84 s are load-bound): no mods 19.44 / 19.81, 13.41 / 17.01, 14.27 / 25.19,
14.07 / 20.50; the 4× pack 14.31 / 18.25, 13.72 / 20.56, 25.45 / 32.38 (a stalled run), 14.05 / 18.41; its Vita pack
14.50 / 20.29, 13.98 / 23.10, 13.68 / 22.28, 14.80 / 19.46. The medians of the four medians: no mods 14.17, the pack
14.18 (1.00×), the Vita pack 14.24 (1.00×); render-thread execute 1.58–1.74 ms without mods, 2.01–2.18 with the
pack, 1.84–2.28 with the Vita pack. Every trace `16ED597F…`.
- **Vita pack:** `omega_mod_tool pack --platform vita` over the 4× pack's folder (30 s on the loaded PC) writes a
  45.3 MB zip: 1012 textures, 599 BC1 and 413 BC3, 36 resized to 1024 px, 1373.8 MB as RGBA8 with mips ->
  176.5 MB of blocks. With it (`local\test_mods\hd_vita`, beside `rt_blender`) the run's trace is the no-mod run's
  (`16ED597F…`), with 667 texture lines and "16 on the GPU, 7 on the CPU" as with the PNG pack; the frames differ
  from the PNG pack's by a mean of 0.03 (frame 100) and 0.17 (frame 290) per channel. Under `--mods-texture-cap 64`
  the level logs `the level's 650 replacement images take 36.0 MB, under the cap` and the frames are byte-identical to
  the uncapped run's. `validate`: TORONTO1 661 images, 40.6 MB at full size (the PNG pack: 302.3 MB, scaled to
  128 px: 56.0 MB); exit 0.
- **Core tests:** `tests/gfx_texture_blocks_tests.cpp` (BC1 / BC3 encode and decode bounds, exact alpha ends,
  partial edge blocks, DDS write / read round trips with the chain, `image_size`'s block bytes, `platform_texture`'s
  sizes, levels, format choice, limit and determinism, `resample_image`, `upload_levels` on device stubs with and
  without `block_textures`, a non-multiple-of-4 base decoded, size mismatches refused);
  `tests/mod_tests.cpp` `test_mod_block_textures` (a BC DDS replacement kept as blocks with the whole chain, decoded
  without it, counted as blocks under the cap with its top levels dropped; raw alpha decoded);
  `test_scene_overlay_buffer_resize` (the buffer rule); `tests/model_tests.cpp` `test_model_auto_lod` (the simplifier keeps outlines, seams and orientation; the level choice). 662 tests (on origin/main e5868a30).
- **Neutrality:** with no mods the 8 play regressions are Identical, the restart revives at frame 212 and the level-end chain ends at frame 8735.

## Evidence (132md12)

- **Stems:** `tests/mod_tests.cpp` `test_mod_wide_backgrounds` checks `override_stem` and `manifest_declares` (a
  `.png` item covers the `.dds`, not the next frame; prefixes as written) and a mod with `overrides=textures/.../A.TDX.png`
  whose folder holds `A.TDX.dds` (used, not ignored; an undeclared `B.TDX.dds` beside it ignored).
- **Wide backgrounds:** the same test parses `wide.` keys (bad extensions, absolute and `..` paths, empty values
  refused), indexes and shadows them across mods (messages, `conflicts`), falls back from `loading.toronto1` to
  `loading`, decodes the images with their notes, and checks the picker (nearest aspect, wider on a tie, 4:3 and
  narrower skipped) and the rect; `tests/host_window_tests.cpp` `test_gui_wide_background` draws a two-colour
  16:9 image on an 800x450 WARP window behind a 4:3 screen whose texture is overridden by an HD image (the bars
  show the image's halves, the 4:3 rectangle the override), returns false without mods, at 4:3, for an unknown
  screen and with the frame fit off, and draws the same pixels after `reset_device`. The committed example scans
  clean in the test. 664 tests.
- **The example:** the mod tool outputs in "Making an HD texture pack"; a copy with a 1920x1080 wide image packs for
  the Vita as 960x540 (`validate`: `wide.loading wide/loading_16x9.png <- big: 960x540`).
- **Neutrality:** no game code calls the hook yet and stems only apply to mods with `overrides=`; with no mods the
  8 play regressions are Identical, the restart revives at frame 212 and the level-end chain ends at frame 8735.

## Evidence (132md13)

- **Tests** (`tests/mod_audio_tests.cpp` `test_mod_audio_overrides`; 668 tests pass after the rebase): the manifest (items, repeated lines,
  `\` paths, bad targets / files / bare numbers / volumes refused), `audio_target_match` (full keys, key ends at a
  `/` only, sound names in any case), the resolution over three mods (sample number over sound name, longer target,
  `sounds\` first within a mod, the first mod over later ones, shadowing and its messages, missing files ignored),
  the entries and conflicts, volumes (manifest, `mod_volume.` settings and `set_mod_volume`), the gain (exactly the
  ungained samples at 1, scaled and clamped otherwise, `FittedAudio` equal to `fit_audio` at every gain),
  `convert_audio_file` (rate / channel caps, an Ogg as a WAV, refusals), and the header-only stream parsers the
  preload and the tool's catalogue use (`parse_vpk_header` / `parse_vag_header`). `tests/mod_tests.cpp` scans the
  committed examples (`hd_pack`, `audio_pack`) and resolves the example's two targets.
- **The example in the game:** TORONTO1 with `--mods tools\mods\examples --audio-wav` (`tools\run_toronto1_level_end.ps1
  -Frames 400`): the log lines above (the music decoded at load and played from frame 2, the alarm sample encoded
  with the bank); the trace equals the run without mods, and the mixed audio differs from frame 1 on (the music).
- **The tool:** `list`, `check-sounds`, `validate audio_pack` (0 errors, 0 warnings, 1 note), `volume audio_pack 50`
  (`mod_volume.audio_pack=50`, `list`: `audio volume: 50% (mod_volume.audio_pack= in the settings; the manifest's
  80%)`), `pack --platform vita` (above; the archive validates in place with the same results), and a test mod with
  an `.ogg` target packed for the Vita as `.wav` with its `audio_overrides=` lines rewritten.
- **Neutrality:** with no mod sounds nothing new runs (the preload returns before reading anything, `audio()` is not
  reached); with no mods the 8 play regressions are Identical, the restart revives at frame 212 and the level-end
  chain ends at frame 8735.

## Evidence (132md14)

- **Tests** (`tests/mod_movie_tests.cpp` `test_mod_movie_overrides`; 669 tests pass): the manifest (`movie_overrides=`
  items in any order, `.PSS` optional, repeated keys accumulating, bad targets / kinds / missing files refused with
  their messages), the resolution over mods (the first mod wins each of picture / sound / subtitles, conflicts keyed
  `movies/INTRO.PSS`), the elementary-stream header (size, frame-rate code, display aspect), the pacing (pull k shows
  picture floor(k * fps * 1001 / 30000); the sound through pull k is (k + 1) * 48000 * 1001 / 30000 frames), a
  synthetic MPEG-2 clip decoded through `ModMovie` (every picture, the end, the audio converted to 48 kHz stereo and
  scaled by the mod's volume), a `.pss` replacement through the disc's demuxer, and the MODS page's order (`#1 OFF`,
  `#1 ON (NEXT)`, Left / Right moves). `tests/mod_tests.cpp` scans the three committed examples (`movie_pack` resolves
  `movie("INTRO.PSS")`).
- **The tool:** `validate --mods tools\mods\examples movie_pack`: 0 errors, 0 warnings, 3 notes (wider than 4:3, 23.976
  fps shown at 29.97, 3.00 s against the original's 172.77 s; the original's length is counted from the disc's
  demuxed video in 4 MB pieces, so the 91 MB movie is never read whole). `pack --platform vita`: "1 movie(s) kept as
  MPEG-2: INTRO.PSS 256x144", the archive's manifest has `platform=vita` and the example's `notes.vita=`.
- **The example in the front end** (`--frontend --frontend-only --mods tools\mods\examples`): the log says
  `movie INTRO.PSS <- movie_pack: picture intro_wide.m2v 256x144 23.976 fps 16:9, sound intro.wav` and
  `movie subtitles INTRO.SUB <- movie_pack: intro.sub`; the mod's cue shows at frame 14 and hides at frame 170 (the
  session's subtitle clock, unchanged), the movie ends at frame 182 and the title screen follows at frame 183 (the
  original's end-of-movie path). At 4:3 the frame-60 capture has the picture letterboxed 16:9 under its subtitle; with
  `--aspect wide --window-size 1280 720` the picture fills the window (`--movie-fill bars` and `zoom` give the same
  image for a 16:9 movie in a 16:9 window, as they should).
- **Neutrality:** with no mod movies `open_movie` returns the disc's `PssMovie` as before (the session's audio goes
  through `PssMovie::audio_format` / `take_audio`, the old inline code moved behind the interface), `movie_list(0)` is
  the old quad and the renderer's wide-aspect branch needs `port_picture_aspect` > 0, which only a mod movie sets. With
  no mods the disc intro plays as before (the movie golden CRC tests pass; the no-mod front-end run shows
  `MOVIE_INTRO.PSS` and its subtitles at frame 302), the 8 play regressions are Identical, the restart revives at
  frame 212 and the level-end chain ends at frame 8735.

## Evidence (132md15)

- **Tests** (671 pass): `tests/platform_movie_tests.cpp` `test_platform_movies`: a stub backend behind `ModMovie`
  (the picture times paced onto the 29.97 clock: repeats for 24 fps, drops for 60 fps; the sound aligned to the
  first picture and resampled to 48 kHz; the end one interval after the last picture; the mod's volume), a movie read
  from an archive member in place, `pack` storing movies, the refusals (no backend: the disc's movie; a picture past
  1920 x 1152; a backend that throws), and on Windows a Media Foundation round trip: the test writes a 320 x 180
  H.264 + AAC MP4 with the Sink Writer (30 pictures at 24 fps, 1.25 s of a tone), decodes it through the backend
  (size, aspect, codecs, every picture's colour and time, the sound's length) and plays it through `ModMovie` at 50%
  volume (38 pulls). `tests/mod_boundary_tests.cpp` `test_mod_level_boundary`: the signature is equal on a rescan,
  and changes for a mod turned off, a volume, a priority move, a used file's edit and a mod added, not for a
  shadowed file's edit; `install` keeps the replaced set alive (a `weak_ptr` to it stays valid), the old `active()`
  pointer stays readable, the generation rises and the winner changes.
- **An MP4 in the front end** (`local\md15_mods\mp4_intro`, the test's movie as `movie_overrides=INTRO=movie/intro.mp4`;
  `--frontend --frontend-only --mods local\md15_mods`): the log says `movie INTRO.PSS <- mp4_intro: picture intro.mp4
  320x180 H.264 24.000 fps 16:9 (Media Foundation), sound intro.mp4's own (AAC 44100 Hz x2)`; the frame-20 capture
  shows the picture letterboxed 16:9 in the 4:3 frame; the movie ends after its 1.25 s and the title screen follows.
  `validate`: 0 errors, 0 warnings, 3 notes (wider than 4:3, 24 fps shown at 29.97, 1.25 s against 172.77 s).
  `pack --platform vita`: the MP4 kept (stored, 31518 bytes) with the `sceAvPlayer` note.
- **A mod change at the next level** (`run_toronto1_level_end.ps1 -Next` with `--mods` (the HD and audio examples)
  and `--port-settings`, the play script `toronto1_port_settings_mods.txt` opening the pause menu's MODS page):
  turning `audio_pack` OFF at frame 357 writes `mod.audio_pack=0` (the page is played in TORONTO1, so the first hand-off, to TORONTO1, finds
  the set unchanged and prints nothing); at the hand-off to TORONTO2 `skipped audio_pack: disabled in the settings` and `applied at the level boundary before
  TORONTO2: 1 mod(s) active (66 ms)`; TORONTO2 sets up and plays 200 frames with no audio overrides. The reverse
  (starting with `mod.audio_pack=0`, turning it ON on the page): `applied ... 2 mod(s) active (23 ms)`, and
  TORONTO2's load uses the audio pack (its bank and music lines follow).
- **Neutrality:** with no mods nothing new runs: no MP4 is opened, and the boundary check is off (no `--mods`, no
  settings file). The 8 play regressions are Identical, the restart revives at frame 212 and the level-end chain
  ends at frame 8735 (logs in the delivery).

## Evidence (132md16)

- **Unit tests** (`tests/mod_data_tests.cpp`; and the gameplay cases in `tests/lan_mod_gate_tests.cpp` and
  `tests/mod_online_tests.cpp`): the parser and each refusal (unknown section, table or field, a field the section
  does not take, wrong type, out of range, twice); the manifest key; the set's resolution, shadowing and ignored
  files; a synthetic `WEAPONS.WDB` (the damage-type x5 rule, by id and by display key) and `DATA.POP` (player and AI
  records, id over level percent over `[npc * *]`); `[tohit]`; the gameplay fingerprint (a cosmetic change keeps it,
  a data value changes it); the report's trailing field (an older report decodes as unknown); `gameplay_refusal`;
  the LAN gate and a real `OmegaServer` under the gameplay policy (the same set joins, another is refused with the
  notice, cosmetic and original sessions join a game without gameplay mods). With the game's ISO: the committed
  example through `DiscImage::file` (record 112: 150 / 150 / 150 damage, clip 15; every other record the disc's;
  an unpatched level's `DATA.POP` the disc's extent; uninstalled: the disc's file). 674 passed.
- **The play test** (TORONTO1, the silenced pistol in the hand, `toronto1_blood_decal.txt`, 420 frames):

  | | Loadout | First hit (frame 132) | NPC 133140 | HUD at frame 419 |
  |---|---|---|---|---|
  | the disc | `item 112 12/120` | `amount 80`, health 20 left | killed at frame 218 (a second blow) | `9/120` |
  | the example (enabled) | `item 112 15/150` | `amount 150`, health 0 | killed at frame 132, the first hit | `12/150` |

  The modded run logs `Mods (port-only): data: WEAPONS.WDB: 4 field(s) from stronger_silenced_pistol (... damage_mid
  80 -> 150, ... clip 12 -> 15)`.
- **The tool:** `validate` on the example: 4 values (80 -> 150 three times, 12 -> 15), 0 errors; on a broken pair:
  the out-of-range file an ERROR (exit 1), three unused selectors warnings, `[npc * *] armor_percent=50` listing only
  the armors it changes. `report` on the enabled example: class gameplay, 4 gameplay entries, the gameplay
  fingerprint, and what each policy does.
- **Neutrality:** with no mods the 8 play regressions are Identical, the restart revives at frame 212 (level pass 1)
  and the chain ends its levels at frames 720, 8735 and 8821, as before.

## Platforms (design)

A later manifest key (`platforms=`) and per-platform folders (`mods\<mod>\platform\<vita|ps4|ps5|switch>\...`,
resolved before the common files) give per-platform variants and LODs. The key layout above already leaves room
for both. The texture memory cap ("Texture memory", 132md9) is the per-platform size limit that exists now: it scales
one set of HD images per platform at load (a Vita build defaults to 64 MB), so a pack needs no Vita-sized copy; a
per-platform folder would still let an author ship hand-made smaller images instead of the box filter's. Since 132md10
a manifest's `notes.<platform>=` lines carry per-platform advice to players (the launcher shows this platform's
first), and the cap reaches model images and files\ textures too. Since 132md11 `omega_mod_tool pack --platform vita`
writes a Vita copy of a pack (BC1 / BC3 with mips, power-of-two, `platform=vita` in its manifest; "Mods on the
Vita"), which fits the Vita's cap at full size and loads on the PC too; the backends say whether they take the blocks
through `gfx::Device::capabilities().block_textures`, and a backend without it gets them decoded.
