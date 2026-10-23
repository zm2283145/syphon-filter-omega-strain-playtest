#!/usr/bin/env python3
"""Port-only (checkpoint 132md1, research/MODDING.md): export a starting point for a mod from YOUR disc image.

Wraps omega_mod_tool (built with the port) with presets:
  python tools/mod_export.py level TORONTO1 --out mods/my_toronto      textures of a mission (TEX, MAPTEX, NPCTEX,
                                                                       OBJECTS, WPN archives)
  python tools/mod_export.py character --match GABE --out mods/gabe    character textures (COMMON MANTEX / MORPHTEX,
                                                                       FRONTEND MANTEX), names containing the text
  python tools/mod_export.py gun --out mods/guns                       weapon textures (COMMON WPNHOG)
  python tools/mod_export.py gui --out mods/gui                        fonts, loading screens, interface art
  python tools/mod_export.py textures GAMEDATA/COMMON/PAR.HOG --out mods/fx   any disc files / archives
  python tools/mod_export.py files GAMEDATA/TORONTO1/SCRIPTS.HOG/X.SCR --out mods/x   raw bytes (same-format edits)
  python tools/mod_export.py model GAMEDATA/UKRAINE/SKL.HOG/UKRAINE_GABE.SKL --out mods/gabe
                                                                       a character's skeleton and parts as glTF 2.0
                                                                       (checkpoint 132md2: models/<part>.SKM.gltf to
                                                                       replace, reference/<name>.gltf the whole body);
                                                                       also part paths (X.SKM or X.SKM=Y.TDX) with
                                                                       --skeleton MALE.SKEL / FEMALE.SKEL
  python tools/mod_export.py level-models TORONTO1 --match SUBWAY_A --out mods/subway
                                                                       a mission's level models as glTF 2.0 (132md2
                                                                       "Stage 3b": terrain pieces and sky in DATA.HOG,
                                                                       objects in OBJECTS.HOG, 132md4: the HUD map's
                                                                       models in MAPVUM.HOG; models/<key>.VUM.gltf
                                                                       with their textures; --match filters the keys)
  python tools/mod_export.py gun-models [TORONTO1] --match BERETTA --out mods/guns
                                                                       gun models as glTF 2.0 (132md3 "Gun models":
                                                                       COMMON WPNHOG.HOG, and the mission's WPN.HOG when
                                                                       one is named; models/<key>.SKM.gltf per part
                                                                       rigged to the gun's 11 bones, reference/<gun>.gltf;
                                                                       --match filters the guns)
  python tools/mod_export.py sounds [TORONTO1] --match THEME --out mods/music
                                                                       voice and music streams as WAV (132md4 "Sounds
                                                                       and music": ZMEDIA MUSIC.HOG and SNDVAG.HOG,
                                                                       FRONTEND SNDVAG.HOG, and the mission's SNDVAG.HOG
                                                                       when one is named; sounds/<key>.wav to replace
                                                                       with a .wav or .ogg; --match filters the keys),
                                                                       and the bank samples (132md5 "Sound effects in
                                                                       the banks": FRONTEND SND.HOG and the mission's)
  python tools/mod_export.py list [--mods mods]                        what the installed mods override

Everything exported is the game's own data read from your disc image: keep it local (the mods folder and local/
are ignored by git) and do not redistribute it. A new mod folder starts disabled (manifest.txt enabled=0): delete
what you do not change, edit the rest, then set enabled=1.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL_CANDIDATES = [
    ROOT / "local" / "build-native" / "Release" / "omega_mod_tool.exe",
    ROOT / "omega_mod_tool.exe",   # a packaged build keeps the tools next to the game
    Path(__file__).resolve().parent / "omega_mod_tool.exe",
]

PRESETS = {
    "level": lambda mission: [f"GAMEDATA/{mission}/{hog}.HOG" for hog in ("TEX", "MAPTEX", "NPCTEX", "OBJECTS", "WPN")],
    "character": lambda _: ["GAMEDATA/COMMON/MANTEX.HOG", "GAMEDATA/COMMON/MORPHTEX.HOG", "GAMEDATA/FRONTEND/MANTEX.HOG"],
    "gun": lambda _: ["GAMEDATA/COMMON/WPNHOG.HOG"],
    "level-models": lambda mission: [f"GAMEDATA/{mission}/{hog}.HOG" for hog in ("DATA", "OBJECTS", "MAPVUM")],
    "gun-models": lambda mission: ["GAMEDATA/COMMON/WPNHOG.HOG"] + ([f"GAMEDATA/{mission}/WPN.HOG"] if mission else []),
    "gui": lambda _: ["GAMEDATA/COMMON/FONTS.HOG", "GAMEDATA/COMMON/LOADING.HOG", "GAMEDATA/COMMON/ERROR.TDX",
                      "GAMEDATA/FRONTEND/NTSC.HOG"],
    "sounds": lambda mission: ["ZMEDIA/MUSIC.HOG", "ZMEDIA/SNDVAG.HOG", "GAMEDATA/FRONTEND/SNDVAG.HOG",
                               "GAMEDATA/FRONTEND/SND.HOG"]
                              + ([f"GAMEDATA/{mission}/SNDVAG.HOG", f"GAMEDATA/{mission}/SND.HOG"] if mission else []),
}


def find_tool(explicit: str | None) -> Path:
    for path in ([Path(explicit)] if explicit else TOOL_CANDIDATES):
        if path.is_file():
            return path
    sys.exit("omega_mod_tool.exe not found (build the port, or give --tool <path>)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=[*PRESETS, "textures", "files", "members", "model", "list"])
    parser.add_argument("paths", nargs="*", help="the mission (level), or disc paths (textures / files / members / model)")
    parser.add_argument("--out", help="the mod folder to write (e.g. mods/my_mod)")
    parser.add_argument("--iso", help="your disc image (default: found next to the game / repository)")
    parser.add_argument("--match", help="only textures (level / gun models, sounds) whose key (name) contains this text")
    parser.add_argument("--by-hash", action="store_true", help="name images by content hash (textures/by-hash)")
    parser.add_argument("--skeleton", help="the skeleton for model part paths (MALE.SKEL / FEMALE.SKEL)")
    parser.add_argument("--mods", help="the mods folder (list)")
    parser.add_argument("--tool", help="omega_mod_tool.exe")
    args = parser.parse_args()

    tool = find_tool(args.tool)
    if args.what == "list":
        command = [str(tool), "list"] + (["--mods", args.mods] if args.mods else [])
    else:
        if args.what in ("level", "level-models"):
            if len(args.paths) != 1:
                parser.error(f"{args.what} takes one mission name, e.g. TORONTO1")
            paths = PRESETS[args.what](args.paths[0].upper())
        elif args.what in ("gun-models", "sounds"):
            if len(args.paths) > 1:
                parser.error(f"{args.what} takes at most one mission name, e.g. TORONTO1")
            paths = PRESETS[args.what](args.paths[0].upper() if args.paths else None)
        elif args.what in PRESETS:
            paths = PRESETS[args.what](None) + args.paths
        else:
            paths = args.paths
        if not paths:
            parser.error("no disc path")
        mode = {"files": "export-files", "members": "members", "model": "export-model",
                "level-models": "export-model", "gun-models": "export-model",
                "sounds": "export-sounds"}.get(args.what, "export-textures")
        command = [str(tool), mode]
        if mode != "members":
            if not args.out:
                parser.error("--out <mod folder> is required")
            command += ["--out", args.out]
        if args.iso:
            command += ["--iso", args.iso]
        if args.match and mode in ("export-textures", "export-model", "export-sounds"):
            command += ["--match", args.match]
        if args.by_hash and mode == "export-textures":
            command.append("--by-hash")
        if args.skeleton and mode == "export-model":
            command += ["--skeleton", args.skeleton]
        command += paths
    return subprocess.call(command)


if __name__ == "__main__":
    sys.exit(main())
