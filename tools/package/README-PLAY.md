# Syphon Filter: The Omega Strain - native PC port (playing)

This folder is the native PC port of *Syphon Filter: The Omega Strain* (PlayStation 2). It contains **no game data**:
the game's files are read at run time from **your own disc image** of the USA release (SCUS-97264). Everything the
port adds that the PS2 game does not have (the launcher, fullscreen, VSYNC, the CPU partner setting, the keyboard /
mouse bindings, mouse look, the cheats menu, the Port Settings page, mods) is marked *port-only* below.

## What is in this folder

| File | What it is |
|---|---|
| `OmegaStrain.exe` | The game: front end (boot movies, title, menus), loading screens, the levels and the level end, in one window. |
| `OmegaStrain.map` | The exe's symbol map: crash reports (`local\crashes\`) name the failing functions with it. |
| `config\disc_us.json` | The size and SHA-256 of the supported disc image and of its boot executable (used to verify your disc). |
| `local\port_settings.txt` | The port-only settings (commented; see "Settings"). |
| `omega_mod_tool.exe` | The mod tool (port-only): exports textures, files, character, level and gun models from your disc image as a starting point for a mod, and checks a mods folder's models (see "Mods"). |
| `MSVCP140*.dll`, `VCRUNTIME140*.dll` | The Microsoft Visual C++ runtime, so no separate redistributable install is needed. |
| `MANIFEST.txt` | Every packaged file with its size and SHA-256. |

Requirements: 64-bit Windows 10 or 11 with a Direct3D 11 GPU. The shader compiler (`D3DCompiler_47.dll`) and the
C runtime (UCRT) are part of Windows 10 and 11.

## First run (port-only launcher)

1. Put the folder somewhere you can write to (for example `Documents\OmegaStrain`): the settings, the memory card and
   the derived data are written to its `local\` folder.
2. Start `OmegaStrain.exe`. It looks for your disc image in this order:
   1. `--iso <file>` on the command line;
   2. the `iso=` line of `local\port_settings.txt`;
   3. any `*.iso` of the right size in this folder (and two folder levels below it), then in your Downloads,
      Documents and Desktop folders (two levels deep);
   4. otherwise it asks for the file with an Open dialog.
3. The image is verified: its size and SHA-256 must match the USA SCUS-97264 disc (and, when the tables below are
   derived, its boot executable `SCUS_972.64`). This reads the whole image and takes a few seconds, so it is done
   once: the result is saved as the `iso_verified=` line of the settings (the image's path, size, last-write time
   and SHA-256), and later starts skip the hash while the image's path, size and last-write time are unchanged. A
   moved, replaced or touched image is hashed again; `--verify-iso` forces the full check.
4. On the first start with a disc (or a new build of the port) it derives the animation and navigation tables from
   the disc into `local\derived\` (well under a second). Then it writes
   `iso=<path>` and `iso_verified=...` to the settings and starts the game.

A wrong or damaged image is rejected with a message saying which check failed (the size or the SHA-256), and the
launcher asks again. Cancelling exits without starting.

Command line (all optional): `OmegaStrain.exe [--iso <file>] [--fullscreen] [--port-settings <file>] [--verify-iso] [--no-dialogs] [--no-search]`
(any other option is passed on to the game, e.g. the diagnostic `--no-cheat-menu`).
`--no-search` skips the automatic search. `--verify-iso` hashes the image even when `iso_verified=` matches.
`--port-settings <file>` uses another settings file instead of `local\port_settings.txt`. `--no-dialogs` reports problems only in the log and the exit code (2 bad arguments, 3 folder not writable,
4 malformed settings, 5 no valid disc image, 1 any other startup error).

## Controls

A controller works as on the PS2: XInput pads (Xbox), DualShock 3 / 4 and DualSense over USB or Bluetooth.
Pads can be plugged in or out at any time; when the pad in use is unplugged the game shows its own "controller not
detected" prompt and pauses, as the PS2 game does, until a pad is connected or a key is pressed.

The keyboard and mouse are a second controller that is always connected (port-only):

**Menus** (front end, loading screens, level end)

| PS2 button | Keys |
|---|---|
| D-pad | Arrow keys |
| Cross (accept) | Enter, Space |
| Triangle (back) | Esc, Backspace |
| Square | Shift |
| Circle | Ctrl |
| L1 / R1 | Q / E |
| L2 / R2 | 1 / 3 |
| L3 / R3 | Z / C |
| Select | Tab |
| Start | P |

**Play** (the level and its start / pause / loadout menus)

| PS2 input | Keys |
|---|---|
| Left stick (move) | W A S D or arrow keys (hold Shift to walk) |
| Right stick (look) | T F G H, or the mouse (mouse look) |
| Cross | Space |
| Triangle | R |
| Square | J, left mouse button |
| Circle | K |
| L1 | I, right mouse button |
| R1 | U |
| L2 / R2 | Q / E |
| L3 / R3 | X / M |
| Select | Z |
| Start (start menu) | P |
| D-pad | Y / B / V / N (the arrow keys in the in-level menus) |

**Window** (port-only, at any time): **Alt+Enter** or **F11** toggles borderless fullscreen on the window's monitor;
**Alt+F4** or the close button quits (the game shuts down cleanly). On a scaled display (125 %, 150 %, ...) the window
is drawn at the display's real resolution (no blur); its default size grows with the scale, and moving it to a monitor
with another scale resizes it. **F3** (in the menus or a level) shows or hides the frame-time
overlay (`frame_overlay_key`; a key bound to a button with `bind.*` would do both).

**Mouse look** (port-only): while a level is playing in the focused window, the cursor is hidden and kept inside the
window, and moving the mouse moves the right stick. The cursor is released in menus, while paused, at the level end
and when the window loses focus.

**In the background** (port-only): the PS2 game has no pause on focus loss, so the game keeps running while you use
another window, but its sound is silenced (`background_audio=1` keeps it) and the keyboard, mouse and controller are
ignored until you click back in. Pause first (P / Start) if you step away.

**Controllers** (port-only): XInput pads, DualShock 4 and DualSense (USB or Bluetooth) can be plugged in or out at any
time, in the menus or a level. When the pad in use goes and no other is left, the game shows its own "controller not
detected" prompt (`controller_prompt=0` turns it off); plug a pad in or press a key to carry on.

## Settings (`local\port_settings.txt`, port-only)

One `key=value` per line; `#` starts a comment. The game keeps your comments when it writes the file. The game's own
options (sound, controls, subtitles, ...) are on the memory card in `local\frontend_card\`, as on the PS2. Every key
below except `window`, `iso`, `iso_verified`, `partner_friendly_fire`, `partner_loadout`, `partner_pickups`, `mods` and
`mods_folder` can also be changed in the game on the **Port Settings** page (see below), which writes the file at once.

| Key | Values (default) | Also changed by |
|---|---|---|
| `coop_partner` | `0` / `1` (0): the CPU co-op partner in play | Title Options > Control > **CPU PARTNER** |
| `partner_friendly_fire` | `0` / `1` (0): your shots, melee and explosives can hurt the CPU partner (1, as between two players in the original) or not (0) | the settings file only (read at each level start) |
| `partner_loadout` | `auto` / `copy` / `record` (auto): the CPU partner's weapons at the level start: the best primary, sidearm and melee from the weapons your agent has unlocked (auto), exactly the loadout you accepted (copy), or the partner character's own fixed kit (record) | the settings file only (read at each level start) |
| `partner_pickups` | `0` / `1` (1): the CPU partner picks up better weapons and ammunition from weapon crates and dropped backpacks | the settings file only (read at each level start) |
| `fullscreen` | `0` / `1` (0): borderless fullscreen | Alt+Enter, F11, `--fullscreen`, Options > Video > **FULLSCREEN**, Port Settings |
| `vsync` | `0` / `1` (1): present on the vertical blank | Options > Video > **VSYNC**, Port Settings |
| `window` | `left,top,right,bottom` (none): the windowed position and size in physical screen pixels (`window_units=pixels`, written with it; an older file without it is converted once) | written when the game closes |
| `iso` | path of your disc image | written by the first run |
| `card_backups` | `0`..`100` (10): memory card backups kept (0: none) | the settings file only |
| `iso_verified` | `<size>;<write time>;<sha256>;<path>`: the last verified image (the hash is skipped while the size, write time and path match) | written after each full check; delete the line or use `--verify-iso` to check again |
| `background_audio` | `0` / `1` (0): keep the sound while the window is in the background | Port Settings (BG AUDIO) |
| `controller_prompt` | `0` / `1` (1): the "controller not detected" prompt | Port Settings (PAD PROMPT) |
| `mouse_look` | `0` / `1` (1) | Port Settings |
| `mouse_sensitivity` | `0.05` .. `20` (1); 24 mouse counts in one frame is a full stick deflection | Port Settings |
| `mouse_invert_y` | `0` / `1` (0): invert the mouse's vertical look | Port Settings |
| `aspect` | `pillarbox` / `4:3` / `stretch` (pillarbox): how the 4:3 picture fills a wider or narrower window; see below | `--aspect`, Port Settings |
| `render_scale` | `0.25` .. `4` (1): the internal resolution as a multiple of the window size (below 1 is faster and softer, above 1 smoother) | `--render-scale`, Port Settings |
| `frame_overlay` | `0` / `1` (0): the frame-time overlay (menus and levels) | the overlay key, `--frame-overlay`, Port Settings |
| `frame_overlay_key` | a key name or `none` (F3): the overlay's on / off key | Port Settings |
| `mods` | `0` / `1` (1): use the mods in the mods folder (0: your disc's assets only); see "Mods" | `--no-mods`; the settings file only (read at start) |
| `mods_folder` | a folder (`mods` next to the game) | `--mods <folder>`; the settings file only |
| `bind.<menu\|play>.<button>` | `<key>[,<key>]` or `none` | Port Settings |
| `online_server` | `<host>[:<port>]` (`127.0.0.1:10075`): the omega_server the Online mode uses (see below) | the settings file only |
| `online_platform_badges` | `0` / `1` (1): the platform badges ("PC", "PS2", ...) next to player names online | the settings file only |
| `online_chat_filter` | `0` / `1` (the server's default): hide rude words in the lobby chat you receive (port-only; the server owner sets the word list) | the settings file only |
| `online_game_password` | text, up to 31 characters (empty): the password other players must type to join the online games you create | the settings file only (`--online-game-password`) |
| `online_game_port` | `0`..`65535` (`3658`, the original game's port): the UDP port of the online games you host; forward it on your router if UPnP does not (see "Hosting behind a home router"). When it is busy, another port is used for that game (logged); `0`: always another | the settings file only (`--online-game-port`) |
| `online_upnp` | `0` / `1` (1): ask your router (UPnP, then PCP, then NAT-PMP) to forward that UDP port while you host a game | the settings file only (`--online-upnp`) |
| `online_relay` | `auto` / `off` / `force` (`auto`): join through omega_server's relay when the direct connection to the host has no answer (only if the server has its relay on); `off` never, `force` always | the settings file only (`--online-relay`) |

Binding buttons: `cross circle square triangle l1 r1 l2 r2 l3 r3 select start up down left right`; in the `play`
profile also `stick_left.<up|down|left|right>`, `stick_right.<up|down|left|right>`, `walk` and
`menu_dpad.<up|down|left|right>`. A value replaces that button's default keys (`bind.play.cross=MouseLeft` also
takes the left mouse button off Square). Key names: `A`..`Z`, `0`..`9`, `F1`..`F24`, `MouseLeft`, `MouseRight`,
`MouseMiddle`, `Mouse4`, `Mouse5`, `Backspace`, `Tab`, `Enter`, `Shift`, `Ctrl`, `Alt`, `Esc`, `Space`, `PageUp`,
`PageDown`, `End`, `Home`, `Left`, `Up`, `Right`, `Down`, `Insert`, `Delete`, `NumMul`, `NumAdd`, `NumSub`,
`NumDecimal`, `NumDiv`, `LShift`, `RShift`, `LCtrl`, `RCtrl`, `Semicolon`, `Equals`, `Comma`, `Minus`, `Period`,
`Slash`, `Grave`, `LBracket`, `Backslash`, `RBracket`, `Quote`. F11, Alt+Enter and Alt+F4 are kept for the window.
On the Port Settings page's key pages Cross replaces a button's keys with the next key pressed, Right adds one and
Left removes the last one.

By default (`aspect=pillarbox`) the game keeps its own 4:3 picture: in a wider or narrower window, and in fullscreen,
menus, movies, loading screens and the HUD are shown at 4:3 with black bars, never stretched, while the 3D view of a
level fills the window. `aspect=4:3` shows only the 4:3 picture everywhere (black bars in levels too, as on a 4:3
television); `aspect=stretch` stretches the 4:3 picture over the whole window. The game's 60 Hz step is the same with
VSYNC on or off and with any of these settings.

**Frame-time overlay (port-only):** F3 in the menus or a level (or `frame_overlay=1`) shows the frame time and frame rate, and the
milliseconds spent in the game simulation (SIM), its mission scripts (SCRIPT), drawing (RENDER) and presenting
(PRESENT), averaged over half a second, at the top left of the picture. Include a screenshot of it with performance
reports.

## CPU partner (port-only)

Title > Options > Control > **CPU PARTNER On** adds a CPU-controlled partner agent to the levels. Off (the default)
plays the original single-player game.

The partner follows you with the game's own NPC navigation. Where you climb a wall, crawl through a pipe, take a
ladder or drop off a ledge, it remembers the spot and does the same there (the original climb, crawl and ladder moves);
it cannot follow jumps. If it is still stuck after about 10 seconds out of your sight, it is moved near you.

**Partner commands.** A few co-op objectives need the two agents at two places at once. In the original the second
agent is another player; here you give the partner an order at a command point:

- **TORONTO1, "Retrieve Broussard's laptop (Team)".** The laptop is inside the bank, behind the fire. Stand in front
  of the bank's entrance (the steps on its west side, outside the fire). While the objective is open the interact
  prompt shows "Press TRIANGLE: send partner to the water main". Press Triangle: the partner runs to the water main
  (the ticker says "Partner: on the way to the water main") and turns it ("Partner: the water main is turned"). The
  sprinklers put the fire out and you have 9 seconds to take the laptop: run through the lobby and the corridor to
  the laptop room and press Triangle at the laptop.
- The order is kept when you walk away, and is cancelled (ticker) if the water main can no longer be turned. You can
  give it again whenever the prompt shows.

**Friendly fire.** By default (`partner_friendly_fire=0` in `local\port_settings.txt`) nothing you or your allies do
hurts the partner: bullets, melee, grenades, explosions and fire you cause. Enemies still hurt it. With
`partner_friendly_fire=1` the original rules apply: two players' bullets do each other no damage, but explosions, fire
and melee do, and killing the partner counts against you in the debrief. The cheat **Super Agent** never makes your hits
on the partner lethal.

**The partner's weapons.** In the original every agent picks its own loadout from its own unlocked weapons. The CPU
partner has no agent of its own, so it uses yours: by default (`partner_loadout=auto`) it takes the strongest unlocked
primary, sidearm and melee weapon (no grenades) and holds the strongest gun; `partner_loadout=copy` gives it exactly the
loadout you accept on the loadout screen; `partner_loadout=record` keeps the partner character's fixed kit. In the
level (`partner_pickups=1`) it walks to a nearby weapon crate or dropped backpack when it is not fighting and takes what
is better for it: a gun with a higher damage rating than the one it carries, a gun for an empty slot, ammunition for
its own guns. You come first: it leaves ammunition (and armour) you need for the same weapon, never takes mission
items, never takes the weapons a level took from you, and never picks up grenades or launchers. It switches between its
guns by range, and to another gun when one runs dry.

## Port Settings page (port-only)

The PS2 game has no such page; the port adds it. Open it from **Title > Options > Control > PORT SETTINGS** (under
CPU PARTNER; opening it leaves the Options without saving them, as backing out does) or from the start menu's last
entry **Port Settings** in a level. It uses the game's own menu look and sounds:

- **DISPLAY**: fullscreen, VSYNC, aspect, render scale, the frame overlay and its key, BG AUDIO (`background_audio`);
- **INPUT**: mouse look, its sensitivity and Y inversion, the pad prompt ("controller not detected"), and RESET KEYS
  (back to the default bindings);
- **PLAY KEYS 1..4** and **MENU KEYS 1..2**: the keyboard / mouse binding of every button.

L1 / R1 (or the PAGE row) turn the page, Left / Right change a value, Cross toggles it. On a key row, Cross waits for
the next key or mouse button you press and makes it the only one, Right adds the next one to the list and Left removes
the last one (Esc cancels; F11 stays the fullscreen key). On RENDER SCALE and SENSITIVITY, Cross lets you type a value
(digits and ".", Backspace, Enter sets it, Esc cancels). Changes apply at once and are
written to `local\port_settings.txt` straight away, keeping its comments.

## Cheats (port-only)

- **In a level:** the start menu (P / Start) has an extra **Cheats** entry: Infinite Health, Infinite Armour,
  Infinite Ammo, End Level, Super Agent, NPCs Ignore You and Unlock All, each On / Off. All cheats are off at the start
  of every level.
- **Title Options:** the extra entries **CHEAT: ALL** (every award, part and weapon) and **CHEAT: WEAPONS** (every
  loadout weapon) change the agent saved on the memory card.

Without them the game is the original.

## Mods (port-only)

A mod replaces some of the game's assets: textures (at any resolution), or any file or archive member of the disc,
replaced by a file of the same format. The game's data still comes from your disc image. Anything no mod supplies is
the original, so a mod can be as small as one picture. The PS2 game has no mods; this is a port-only addition.

**Install a mod:** put its folder into `mods\` next to `OmegaStrain.exe` (create `mods\` if it is not there) and start
the game. `local\omega_strain.log` lists the active mods and every asset they replace (lines starting with `Mods
(port-only):`). A broken image or a mistake in a mod is logged and the original is used. To turn every mod off, set
`mods=0` in `local\port_settings.txt`; to disable one mod, set `enabled=0` in its `manifest.txt`.

**Manage mods:** hold **Shift** while `OmegaStrain.exe` starts (or start it with `--mod-panel`) to open the mod panel
before the game: it lists every mod folder, and its buttons enable or disable a mod, move it up or down in the
load order (the first one wins where two replace the same asset), turn all mods off, open the mods folder, and show
where mods overlap. **Play** starts the game; **Close** quits. The in-game Port Settings page has the same switches on
its MODS page.

**Make a mod:**

1. Export a starting point from your own disc image:

   ```
   omega_mod_tool export-textures --out mods\my_mod GAMEDATA/TORONTO1/TEX.HOG
   omega_mod_tool export-textures --match GABE --out mods\my_mod GAMEDATA/COMMON/MANTEX.HOG
   omega_mod_tool members GAMEDATA/COMMON/MANTEX.HOG
   ```

   The tool uses the disc image of your `iso=` setting. It writes each texture as a PNG at
   `mods\my_mod\textures\<disc path>\<member>.TDX.png`, a `textures_index.csv` (sizes and content hashes) and a
   `manifest.txt` with `enabled=0`. Useful disc paths:

   | What | Disc paths |
   |---|---|
   | a level's textures | `GAMEDATA/<LEVEL>/TEX.HOG`, `NPCTEX.HOG`, `OBJECTS.HOG`, `MAPTEX.HOG` |
   | the agents' clothes and faces | `GAMEDATA/COMMON/MANTEX.HOG`, `MORPHTEX.HOG` |
   | the guns | `GAMEDATA/COMMON/WPNHOG.HOG` |
   | the interface | `GAMEDATA/COMMON/FONTS.HOG`, `LOADING.HOG`, `GAMEDATA/FRONTEND/NTSC.HOG` |

2. Edit the PNGs you want to change, at any size (twice or four times the original works well; keep the aspect
   ratio, since the picture is stretched over the original's area). Alpha 255 is opaque. Delete the PNGs you did
   not change, so other mods can replace them.
3. Edit `manifest.txt`:

   ```
   name=My mod
   version=1.0
   author=you
   priority=0
   enabled=1
   ```

   When two mods replace the same asset, the higher `priority` wins (then the folder name).
4. Start the game and check the log.

`files\<disc path>\<member>` replaces a whole file or archive member: `omega_mod_tool export-files --out mods\my_mod
GAMEDATA/COMMON/MANTEX.HOG/<member>` writes the original to start from. Replacing scripts, populations or animation
changes how the game plays. In LAN games every player needs the same mods.

**Do not share game data.** Everything exported is the game's own data from your disc. Share only what you made
yourself.

**Character models.** `omega_mod_tool export-model --out mods\my_mod GAMEDATA/UKRAINE/SKL.HOG/UKRAINE_GABE.SKL` writes
a character's body parts as glTF 2.0 models rigged to the game's skeleton, at
`mods\my_mod\models\<disc path>\<part>.SKM.gltf` (with its `.bin` and `.png`), plus the whole body in
`mods\my_mod\reference\` for viewing. Other characters: `GAMEDATA/TOKYO/SKL.HOG/LIAN_COMBAT.SKL` (Lian),
`GAMEDATA/TORONTO1/SKL.HOG/PC_MALE.SKL` (the agent); single parts by name, e.g.
`omega_mod_tool export-model --out mods\my_mod 2HAIR_A01_HAIR0.SKM=2HAIR_A01_BROW0.TDX`. Edit a part in a 3D
editor such as Blender (import glTF, keep the armature and its bone names `bone_00` ...; reshape, add detail, repaint
at any resolution; export glTF with the skin), and keep the file name. Bones cannot be moved or renamed: the game
checks the skeleton, logs any difference and draws the original part. A glTF with the armature but no mesh hides
the part. The animation, the hit areas and the gameplay are unchanged. Face and hand parts are exported with their
expressions, speech and hand poses as shape keys (glTF morph targets, named `EAT`, `EMO_SHOT`, `CGRIP` ...); keep
the shape keys and their names when you reshape them, and the game keeps moving your face and hands. Without shape
keys the replaced part stays still (the log says so). The same replacements are drawn on the agent figure of the
menus: the new-agent and agent-edit screens, the dossier and the level-start loadout screen.

**Level models.** `omega_mod_tool export-model --out mods\my_mod GAMEDATA/TORONTO1/DATA.HOG GAMEDATA/TORONTO1/OBJECTS.HOG`
(add `--match TEXT` to keep only the names containing TEXT) writes a level's terrain pieces, sky and objects as glTF 2.0
at `mods\my_mod\models\<disc path>\<model>.VUM.gltf`, with their textures as PNG. Edit one, keep its file name and its
material names (`material_03` draws with the game's material 3; any image you give it replaces that texture, at any
resolution) and its place in the level. Only the drawing changes: you walk on, shoot and hide behind the original
shape. A glTF with no mesh hides the model. On models whose lights switch their colours, the switching moves to your
model's nearest vertices; animated textures (signs, screens) keep the game's animation and texture. Details:
`research/MODDING.md` in the source repository.

**Gun models.** `omega_mod_tool export-model --out mods\my_mod GAMEDATA/COMMON/WPNHOG.HOG --match BERETTA` writes
each part of the matching guns (body, clip, bolt, holster, muzzle flash) as glTF 2.0 at
`mods\my_mod\models\GAMEDATA\COMMON\WPNHOG.HOG\<gun>.HOG\<part>.SKM.gltf`, rigged to the gun's 11 bones (`bone_00`
is the grip), and the whole gun in `mods\my_mod\reference\`. Edit a part like a character part (keep the armature,
the bone names and the file name; any detail, any texture resolution). The game draws it in the hand, in the
holster, on the loadout screen and in the weapon previews; the muzzle, the shells and the aim stay the original's.
A mission's own guns are in `GAMEDATA/<MISSION>/WPN.HOG` (add it to the command to export them).

**Blender, and checking a mod.** Import the exported glTF with the defaults. Export glTF 2.0 over the same name with
Vertex Color set to "Active", Shape Keys on, Skinning with "Include All Bone Influences", and no animation.
`omega_mod_tool check-models --mods mods` then lists each model your enabled mods replace, accepted or refused with
the reason, without starting the game.

**Higher resolution and worked examples.** Images may be up to 8192 pixels a side (a 4× texture, font page or menu
image draws in the same place with more detail) and character, gun and level parts up to about a million vertices;
the characters and guns are skinned on the CPU each frame, so very dense parts slow the game. A part shared by every
level (the faces, hands and bodies in `GAMEDATA/COMMON`) is exported with the exported level's texture: export one
character of one level per mod. `research/MODDING.md` ("Mod author guide") walks through an HD face and body made in
Blender, a level texture pack, a sound pack and a sharper font, and `tools\make_example_mod.py` in the source writes
a small example mod to copy.

## Saves and the memory card (port-only tools)

- Your saves are in `local\frontend_card\BASCUS-97264\` of the game folder (the PC memory card; one folder, one file per
  save). Back that folder up to keep your progress.
- Before the game changes a save, the whole card folder is copied to `local\frontend_card\backups\<date-time>\`; the
  newest 10 are kept (`card_backups=` in the settings file; 0 turns it off).
- `OmegaStrain.exe --card-restore local\frontend_card\backups\<date-time>` puts a backup back (the current card is backed up first).
- `OmegaStrain.exe --card-export <file.ps2>` writes your saves into a PCSX2 memory card file (a new one, or an existing one
  whose other games' saves are kept; the old file is first copied to `<file>.ps2.bak`).
  `OmegaStrain.exe --card-import <file.ps2>` copies the game's saves from a PCSX2 card (e.g. `Mcd001.ps2`) onto the PC card
  (add `--card-replace` to also remove PC saves the card does not have). A message box reports the result.
- An export also writes the save's PS2 browser icon (`icon.sys` and `MEMCARD.ICN`, made from your own disc image, the
  `iso=` setting, as the PS2 game writes them), so the save shows its icon and title in the PS2 / PCSX2 memory card
  browser. Without a known disc image the export goes on without the icon (the log says so).

## Online play and omega_server (port-only, in progress)

Sony's online servers are gone, so the port has its own server, **omega_server** (built from the source as
`local\build-native\Release\omega_server.exe`; design in `research\ONLINE.md` of the source). It keeps the accounts,
the lobby, the game list, chat and the policy / announcement texts; the game itself runs peer to peer between the
players, as on the PS2. The game's Online menu uses it: sign-up and login (ACNTLOGN), the policy and announcement
pages, the lobby's player list and chat, creating a game (optionally with a password) and joining one, after which
the game runs as a LAN game between the players. LAN play needs no server.

- **Run it:** `omega_server.exe` (listens on 127.0.0.1:10075; `--bind 0.0.0.0` for other machines, `--port N`).
  Files, in the folder it runs from: `omega_accounts.txt` (accounts; passwords are stored only as salted PBKDF2
  digests), `omega_policy.txt` (the usage / privacy policy and the announcements, placeholder text on the first run;
  the version line goes up when an admin changes the usage policy, and every player accepts it again),
  `omega_server.log` (connections, logins, refusals, games, chat, admin commands). `--config FILE` reads
  `key=value` lines: `server_name`, `allow_cheats` (1: the port's cheats are allowed online, each player's cheats
  apply only to that player; 0 turns them off), `admin_token`, `max_players`, `port`, `bind`, `public_ip` (the
  server's own address as clients reach it, written into the login reply's connect info; default 127.0.0.1), `accounts`, `policy_file`, `log`; the command line wins
  over the file.
- **Admin:** type commands into the server's console (`help` lists them), or from another console:
  `omega_server.exe admin 127.0.0.1:10075 --token <admin_token> <command>` (or `--user NAME --password PW` for an
  account made admin with `admin NAME on`). Commands: `players`, `games`, `accounts`, `ban NAME 30m|2h|7d|perm
  [reason]` (the player is disconnected and refused at login with the reason), `unban NAME`, `kick NAME [reason]`,
  `close WORLD`, `broadcast TEXT`, `cheats on|off`, `admin NAME on|off`, `password NAME NEW`, `create NAME PW`,
  `policy`, `policy usage|privacy TEXT` (`\n` for a new line), `announce list|add TEXT|del N|clear`, `motd TEXT`.
- **Check it:** `omega_server.exe demo 127.0.0.1:10075` runs scripted test clients against a running server (policy,
  sign-up, login, a hosted and a joined game, chat) and prints the result.
- In the game, `online_server=` in `local\port_settings.txt` names the server (default `127.0.0.1:10075`).
- PS2 consoles and PCSX2 cannot reach it yet (they need the original network encryption and a DNAS bypass; see
  `research\ONLINE.md` §8).

### Hosting behind a home router (port-only)

The level itself runs between the players' PCs: the joiners connect to the host's PC on **UDP 3658** (the original
game's port; `online_game_port=`). A joiner tries the host directly first; if that has no answer and the server runs
its relay (`relay_port=` on omega_server), the game goes through the server instead. `local\omega_strain.log` says
which: `in-level path: direct to the host` or `in-level path: through omega_server's relay`.

- **Automatic (UPnP, PCP, NAT-PMP):** with `online_upnp=1` (the default) the hosting game asks the router to forward
  UDP 3658, trying UPnP first, then PCP and NAT-PMP (which many routers without UPnP, Apple's among them, speak), and
  removes the forward when you leave the game. The log shows `online: port mapping: ...` with the result. The
  router must have one of them turned on.
- **What the game tells you** (one line on the front end's notices and on the in-level HUD ticker):
  - `Connection: UPnP mapped ext port 3658` (or `PCP` / `NAT-PMP`): your router forwards the port; nothing to do;
  - `Connection: port mapping failed - see the port-forward guide (UDP 3658)`: the router answered no method;
    forward the port by hand (below), or the players who join you will need the server's relay;
  - `Connection: hosting on UDP 3658 (port mapping off - see the port-forward guide)`: `online_upnp=0`;
  - `Connection: direct to the host` / `Connection: relay through omega_server`: how you, as a joiner, reached the
    host.
- **By hand** (routers without UPnP, PCP or NAT-PMP), on the hosting player's side only:
  1. give the PC a fixed LAN address (a DHCP reservation in the router);
  2. in the router's port forwarding page, forward **UDP 3658** to that PC's address, port 3658 (no TCP port is
     needed);
  3. allow it in the PC's firewall: Windows asks the first time you host (allow it), or in an administrator
     PowerShell: `New-NetFirewallRule -DisplayName "Omega Strain online game" -Direction Inbound -Protocol UDP -LocalPort 3658 -Action Allow`.
- A connection that is behind a carrier's NAT cannot be forwarded from home: use the server's relay or a VPN.
- Server operators: the relay and its firewall rule are in `research\ONLINE_DEPLOY.md` §2.

## Troubleshooting

- `local\omega_strain.log` has everything the game prints when started from Explorer (from a console, the output goes
  to the console instead), errors included. Include it with any report.
- If the game stops with an error or crashes, a message box names two files in `local\crashes\`: `crash-*.txt` (what
  failed, where, and the last log lines) and `crash-*.dmp` (a minidump). Attach both to the report (port-only crash
  reporter; nothing is sent anywhere). `OmegaStrain.map` next to the exe lets the report name the functions.
- "Not writable": move the folder out of `Program Files` (or any read-only place).
- To use another disc image, delete the `iso=` line or start once with `--iso <file>`.
- To check the disc again, start once with `--verify-iso` (or delete the `iso_verified=` line).
- To reset the port settings, delete `local\port_settings.txt`; to start with an empty memory card, delete
  `local\frontend_card\`.
- If something looks or plays wrong with mods installed, try once with `mods=0` (or `--no-mods`) and include the
  `Mods (port-only):` log lines in the report.

## Known limitations

- Only the USA disc (SCUS-97264) is supported, and the image must be an exact, unmodified 1:1 copy.
- The port is a work in progress: the front end and TORONTO1 (Carthage, Michigan) onward through the level end are
  what is tested; later levels may stop with an error message ("unhosted ..."), which names the part of the original
  game that is not ported yet.
- Later starts trust the saved `iso_verified=` entry while the image's path, size and last-write time are unchanged
  (a content change that keeps all three is not noticed; use `--verify-iso` after editing an image in place).
- Online play is tested on one machine (two games and omega_server on 127.0.0.1); some online screens (the agent
  info in the game room, for example) still stop with an "unhosted" error.
