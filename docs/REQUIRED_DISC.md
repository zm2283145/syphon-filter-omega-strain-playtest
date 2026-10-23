# Required user-supplied disc

The project targets **Syphon Filter: The Omega Strain, PlayStation 2, USA
(NTSC-U), serial SCUS-97264**. Supply your own unmodified disc image. The
repository may contain our own game, engine, platform and tooling source code,
plus research documentation. It must not contain the disc, extracted assets,
PS2 executable, copied game scripts, or raw decompiler/translated output from
the original executable. Users supply the original game content themselves.

Only the following exact image is currently supported:

| Field | Required value |
| --- | --- |
| Image format | ISO9660, 2048-byte sectors |
| Image length | **3,457,744,896 bytes** |
| Image SHA-256 | `a60a4b41fea808335baa3b887a377bb98063f6f169ca498efee49397fba96130` |
| Boot executable | `SCUS_972.64` |
| Executable length | 4,071,592 bytes |
| Executable SHA-256 | `9924da91767c8145411f37fa6c14c9d77208264c17f1ce9ee157d51abdd31dc6` |
| Boot configuration | `VER = 1.00`, `VMODE = NTSC` |

The filename is irrelevant. Region and serial alone are insufficient if another
revision or modified image differs. The `VER` field above is the value in
`SYSTEM.CNF`; it is not independent proof of a mastering/release revision.
The hashes are measured from the supplied disc, not matched to an external dump
database. PAL, Japanese, patched, rebuilt, and differently fingerprinted images
are unsupported by the current recipe.

## Verify and extract

```powershell
python tools/verify_disc.py 'X:\Your Games\Omega Strain.iso'
python tools/inspect_disc.py 'X:\Your Games\Omega Strain.iso'
python tools/inspect_hog.py 'X:\Your Games\Omega Strain.iso'
python tools/script_objects.py 'X:\Your Games\Omega Strain.iso'
python tools/script_bytecode.py
python tools/script_dependencies.py
python tools/trace_objectives.py
```

Each ISO-facing command verifies the full disc fingerprint before producing
output. Keep the ISO wherever convenient; all generated game content is written
under the ignored `local/` directory. No tool downloads game data. The manifest
is [config/disc_us.json](../config/disc_us.json).

There are now native script, texture and 3D geometry research builds, not a playable native game.
`tools/run_native.ps1` builds the Windows console application, which verifies
this ISO fingerprint itself before extracting and executing the selected script.
See the README for commands and exact limits. Source and synthetic tests can
compile without game files, but content-backed prototype execution requires the
user-supplied verified ISO. Decompiler outputs and generated translated game code
remain local research inputs and must not be committed.
The asset viewer uses the same verification gate before decoding textures and
keeps its raw texture, RGBA/BMP output and window captures under ignored `local/`.
The geometry viewer also verifies the full ISO before nested model/COL extraction.
Mesh reports and GPU captures remain under ignored `local/`; no original model,
collision data or VU program is linked into the native executables.

## The launcher (port-only, checkpoint 132win8)

`OmegaStrain.exe` (the release folder made by `tools/package_release.ps1`; it
contains no game data) checks the same fingerprint natively: the image size,
then the full SHA-256 against `config/disc_us.json`. It searches for the image
(`--iso`, the `iso=` port setting, `*.iso` files of the exact size in its folder
and the user's Downloads / Documents / Desktop, then a file dialog), and derives
the motion graphs and the actor / navigation tunings into its own `local/derived`
folder with native ports of `motion_graph_extract.py`, `actor_animation_data.py`
and `actor_navigation_data.py` (byte-identical outputs; the boot executable's
SHA-256 is checked before reading it). A rejected image is reported with the
failed check. See [FRONTEND.md](FRONTEND.md) "Checkpoint 132win8".

## Repository checks

After cloning, enable the supplied hook with:

```powershell
git config core.hooksPath .githooks
```

The hook checks added/modified staged files for disallowed paths, asset types,
generated decompilation names, binary payloads, and unusually large source files.
CI audits every tracked file and runs tests with no game files present. The
local-disc integration tests skip when the user's extracted scripts are absent.
Review staged content as well: no path-based checker can identify every possible
game asset disguised as hand-written source or prose.
