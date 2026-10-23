#!/usr/bin/env python3
"""Port-only (checkpoint 132md6, research/MODDING.md "Mod author guide"): writes a tiny example mod, all synthetic.

Nothing is read from the disc, so the result may be shared or committed as a template. It holds one entry of each
kind a mod needs no export for:
  textures\\GAMEDATA\\TORONTO1\\TEX.HOG\\WHITECONC.TDX.png  a level texture at 4x (256x256 procedural bricks; the
                                                        disc's is 64x64, the white concrete on the walls of the
                                                        regression view's first room)
  textures\\GAMEDATA\\COMMON\\LOADING.HOG\\BAR.TDX.png     an interface image at 4x (the loading bar: a gradient,
                                                        256x256; the disc's is 64x64)
  sounds\\GAMEDATA\\TORONTO1\\SND.HOG\\WEPS.BNK\\13.wav    a bank sound effect (a 990 Hz beep with a decay; sample
                                                        13 of TORONTO1's WEPS.BNK is the 92F's shot)
  manifest.txt                                          name, version, author, description, priority, enabled=1
                                                        and overrides= naming the three archives
Usage: python tools/make_example_mod.py [--out local/example_mods] [--name md6_example] [--priority 5] [--disabled]
       [--hd-pack | --audio-pack | --movie-pack | --data-pack]
Then: omega_mod_tool list --mods local\\example_mods; omega_mod_tool conflicts --mods local\\example_mods;
      omega_mod_tool check-sounds --mods local\\example_mods; omega_play ... --mods local\\example_mods.

Checkpoint 132md12 (research/MODDING.md "Making an HD texture pack"): --hd-pack writes the HD texture pack example
committed as tools/mods/examples/hd_pack (python tools/make_example_mod.py --hd-pack rewrites it byte for byte):
  textures\\GAMEDATA\\COMMON\\LOADING.HOG\\BAR.TDX.png     the loading bar at 4x (256x256)
  textures\\GAMEDATA\\TORONTO1\\TEX.HOG\\WHITECONC.TDX.png  Toronto's white concrete at 2x (128x128 bricks)
  wide\\loading_16x9.png                                 a 16:9 loading-screen background (wide.loading=)
  manifest.txt                                          overrides= naming the two .png files (they still match
                                                        after omega_mod_tool pack --platform vita makes them .dds)

Checkpoint 132md13 (research/MODDING.md "Audio overrides"): --audio-pack writes the audio example committed as
tools/mods/examples/audio_pack (python tools/make_example_mod.py --audio-pack rewrites it byte for byte):
  music\\toronto_amb.wav     a 2 s ambient loop (two slow sines, 16 kHz mono) for TORONTO1_AMB3.VPK, the ambient
                             music Toronto starts with (the game fits it to the stream's 32000 Hz stereo)
  sfx\\alarm.wav             a two-tone alarm (0.5 s, 16 kHz mono) for the O_BANK_ALARM sound of every level's
                             ENVIRON.BNK
  manifest.txt               audio_overrides= naming both (two lines: the key repeats), audio_volume=80

Checkpoint 132md14 (research/MODDING.md "Movie overrides"): --movie-pack writes the movie example committed as
tools/mods/examples/movie_pack (python tools/make_example_mod.py --movie-pack rewrites it byte for byte):
  movie\\intro_wide.m2v     a 3 s MPEG-2 video elementary stream, 256x144 16:9 at 23.976 fps (72 intra pictures of
                             flat 8x8 blocks: a moving bar over a gradient, written by the DC-only encoder below)
  movie\\intro.wav          its sound (3 s, 22050 Hz mono: a slow chord with fades)
  movie\\intro.sub          subtitles in the disc's .SUB form (one English cue)
  manifest.txt               movie_overrides=INTRO=... naming all three (the intro movie the game opens with)

Checkpoint 132md16 (research/MODDING.md "Gameplay data overrides"): --data-pack writes the gameplay data example
committed as tools/mods/examples/stronger_silenced_pistol (rewritten byte for byte; --enabled writes enabled=1 for a
play test, as the committed one ships disabled):
  pistol.ini                 [weapon SocomSilenced]: 150 damage in every band and a 15-round clip
  manifest.txt               data_overrides=pistol.ini
Only the Python standard library is used (the PNG, WAV and MPEG-2 writers are below).
"""

import argparse
import math
import struct
import sys
import wave
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def write_png(path, width, height, pixel):
    """An 8-bit RGBA PNG; pixel(x, y) returns (r, g, b, a)."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def bricks(x, y, size=256, grain_mask=15):
    """Red bricks, 8 courses of 4, offset every other course, with grey mortar and a little deterministic grain."""
    course = size // 8
    row = y // course
    brick = size // 4
    bx = (x + (brick // 2 if row % 2 else 0)) % size
    mortar = y % course < 4 or bx % brick < 4
    grain = ((x * 73856093) ^ (y * 19349663)) & grain_mask
    if mortar:
        return (150 + grain, 148 + grain, 140 + grain, 255)
    shade = 20 * ((bx // brick + row) % 3)
    return (150 + shade + grain, 60 + shade // 2 + grain, 40 + grain, 255)


def bar(x, y, width=256, height=256):
    """The loading bar: a cyan-to-white gradient with a rim."""
    if x < 8 or y < 8 or x >= width - 8 or y >= height - 8:
        return (255, 255, 255, 255)
    t = x / (width - 1)
    return (int(40 + 215 * t), int(200 + 55 * t), 255, 255)


def wide_loading(x, y, width=384, height=216):
    """A 16:9 loading-screen background: a night-blue gradient with faint scan lines and a teal horizon band."""
    t = y / (height - 1)
    if abs(y - height * 2 // 3) < 2:
        return (40, 170, 170, 255)
    line = 6 if y % 6 == 0 else 0
    return (int(8 + 20 * t) + line, int(14 + 40 * t) + line, int(40 + 70 * t) + line, 255)


def write_hd_pack(mod):
    """The HD texture pack example (tools/mods/examples/hd_pack)."""
    write_png(mod / "textures" / "GAMEDATA" / "COMMON" / "LOADING.HOG" / "BAR.TDX.png", 256, 256, bar)
    write_png(mod / "textures" / "GAMEDATA" / "TORONTO1" / "TEX.HOG" / "WHITECONC.TDX.png", 128, 128,
              lambda x, y: bricks(x, y, 128, 3))
    write_png(mod / "wide" / "loading_16x9.png", 384, 216, wide_loading)
    manifest = "\n".join([
        "# The HD texture pack example (tools/make_example_mod.py --hd-pack; research/MODDING.md",
        "# \"Making an HD texture pack\"): all synthetic. Copy this folder into mods\\ (or start omega_play with",
        "# --mods tools\\mods\\examples) and pack it with omega_mod_tool pack [--platform vita].",
        "name=Example HD pack",
        "version=1.0",
        "author=make_example_mod.py",
        "description=A 4x loading bar, 2x bricks on Toronto's white concrete, a 16:9 loading-screen background",
        "game_version=SCUS-97264",
        "priority=5",
        "enabled=1",
        "# The files this pack replaces, named as in the folder: a .png pattern also covers the .dds",
        "# that pack --platform vita converts it to (the stem rule).",
        "overrides=textures/GAMEDATA/COMMON/LOADING.HOG/BAR.TDX.png,textures/GAMEDATA/TORONTO1/TEX.HOG/WHITECONC.TDX.png",
        "# Drawn behind every loading screen when the window is wider than 4:3 (the 4:3 screen on top).",
        "wide.loading=wide/loading_16x9.png",
        "notes.pc=Full size; no texture cap.",
        "notes.vita=Pack with omega_mod_tool pack --platform vita (BC1/BC3 with mips; the wide image kept as a PNG).",
        "",
    ])
    (mod / "manifest.txt").write_text(manifest, encoding="utf-8", newline="\n")


def write_wav(path, rate, samples):
    """A 16-bit mono WAV of samples in -1..1."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, round(32767 * s)))) for s in samples))


def write_audio_pack(mod):
    """The audio example (tools/mods/examples/audio_pack)."""
    rate = 16000
    # An ambient loop: whole cycles of 110 Hz and 165 Hz (2 s) so the loop point is seamless, with a slow swell.
    write_wav(mod / "music" / "toronto_amb.wav", rate,
              [0.25 * math.sin(2 * math.pi * 110 * n / rate) * (0.6 + 0.4 * math.sin(math.pi * n / (2 * rate))) +
               0.15 * math.sin(2 * math.pi * 165 * n / rate) for n in range(2 * rate)])
    # A two-tone alarm: 880 Hz then 660 Hz, a quarter second each, with short fades.
    def alarm(n):
        tone = 880 if n < rate // 4 else 660
        edge = min(n % (rate // 4), rate // 4 - n % (rate // 4), 160) / 160
        return 0.5 * edge * math.sin(2 * math.pi * tone * n / rate)
    write_wav(mod / "sfx" / "alarm.wav", rate, [alarm(n) for n in range(rate // 2)])
    manifest = "\n".join([
        "# The audio example (tools/make_example_mod.py --audio-pack; research/MODDING.md \"Audio overrides\"): all",
        "# synthetic. Copy this folder into mods\\ (or start omega_play with --mods tools\\mods\\examples); check it with",
        "# omega_mod_tool check-sounds / validate; pack it with omega_mod_tool pack [--platform vita].",
        "name=Example audio pack",
        "version=1.0",
        "author=make_example_mod.py",
        "description=An ambient loop for Toronto's opening music and a two-tone alarm for every level's bank alarm",
        "game_version=SCUS-97264",
        "priority=5",
        "enabled=1",
        "# <target>=<file>: a target is a disc sound's key, or its end at a '/' (every HOG holding it), or",
        "# <bank>/<sound name> (the bank samples that sound plays). The key may repeat; the items accumulate.",
        "audio_overrides=TORONTO1_AMB3.VPK=music/toronto_amb.wav",
        "audio_overrides=ENVIRON.BNK/O_BANK_ALARM=sfx/alarm.wav",
        "# Every sound of this mod at 80% (the player's mod_volume.audio_pack= in the settings wins).",
        "audio_volume=80",
        "notes.vita=Pack with omega_mod_tool pack --platform vita (the files resampled to the game's rates).",
        "",
    ])
    (mod / "manifest.txt").write_text(manifest, encoding="utf-8", newline="\n")


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.bits = 0

    def put(self, value, count):
        for i in range(count - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.bits += 1
            if self.bits == 8:
                self.out.append(self.acc)
                self.acc = 0
                self.bits = 0

    def code(self, text):
        for c in text:
            self.put(1 if c == "1" else 0, 1)

    def start(self, code):
        """Zero bits to the byte, then the start code 00 00 01 <code>."""
        if self.bits:
            self.put(0, 8 - self.bits)
        self.out += bytes((0, 0, 1, code))


# ISO/IEC 13818-2 tables B.12 / B.13: dct_dc_size_luminance / _chrominance by size 0..11.
DC_LUMA = ["100", "00", "01", "101", "110", "1110", "11110", "111110", "1111110", "11111110", "111111110", "111111111"]
DC_CHROMA = ["00", "01", "10", "110", "1110", "11110", "111110", "1111110", "11111110", "111111110", "1111111110",
             "1111111111"]


def mpeg2_dc_video(width, height, aspect_code, rate_code, pictures, block):
    """An MPEG-2 video elementary stream (main profile, 4:2:0, progressive) of intra pictures whose 8x8 blocks are
    flat: only each block's DC coefficient is coded (8-bit precision, so a block's samples are its DC value).
    block(k, plane, bx, by) is picture k's value (0..255) for luma (plane 0) block (bx, by) or chroma block (plane 1:
    Cb, 2: Cr; half the luma's blocks each way)."""
    assert width % 16 == 0 and height % 16 == 0
    w = BitWriter()
    for k in range(pictures):
        if k == 0:
            w.start(0xB3)                    # sequence header
            w.put(width, 12)
            w.put(height, 12)
            w.put(aspect_code, 4)
            w.put(rate_code, 4)
            w.put(0x3FFFF, 18)               # bit_rate_value: variable
            w.put(1, 1)
            w.put(112, 10)                   # vbv_buffer_size_value
            w.put(0, 3)                      # constrained, no matrices
            w.start(0xB5)                    # sequence extension
            w.put(1, 4)
            w.put(0x48, 8)                   # main profile, main level
            w.put(1, 1)                      # progressive_sequence
            w.put(1, 2)                      # 4:2:0
            w.put(0, 2 + 2 + 12)
            w.put(1, 1)
            w.put(0, 8 + 1 + 2 + 5)
            w.start(0xB8)                    # group of pictures: closed
            w.put(0, 1)
            w.put(0, 5 + 6)
            w.put(1, 1)
            w.put(0, 6 + 6)
            w.put(1, 1)
            w.put(0, 1)
        w.start(0x00)                        # picture header: I
        w.put(k % 1024, 10)
        w.put(1, 3)
        w.put(0xFFFF, 16)
        w.put(0, 1)
        w.start(0xB5)                        # picture coding extension
        w.put(8, 4)
        w.put(0xFFFF, 16)                    # f_codes 15
        w.put(0, 2)                          # intra_dc_precision: 8 bits
        w.put(3, 2)                          # frame picture
        w.code("0100")                       # top_field_first 0, frame_pred_frame_dct 1, concealment 0, q_scale_type 0
        w.code("000")                        # intra_vlc_format 0, alternate_scan 0, repeat_first_field 0
        w.code("110")                        # chroma_420_type 1, progressive_frame 1, composite_display 0
        for row in range(height // 16):
            w.start(row + 1)                 # slice
            w.put(8, 5)                      # quantiser_scale_code (unused: no AC coefficients)
            w.put(0, 1)
            predictor = [128, 128, 128]
            for col in range(width // 16):
                w.code("1")                  # macroblock_address_increment 1
                w.code("1")                  # macroblock_type: intra
                blocks = [(0, 2 * col, 2 * row), (0, 2 * col + 1, 2 * row), (0, 2 * col, 2 * row + 1),
                          (0, 2 * col + 1, 2 * row + 1), (1, col, row), (2, col, row)]
                for plane, bx, by in blocks:
                    value = max(0, min(255, int(block(k, plane, bx, by))))
                    diff = value - predictor[plane]
                    predictor[plane] = value
                    size = abs(diff).bit_length()
                    w.code((DC_LUMA if plane == 0 else DC_CHROMA)[size])
                    if size:
                        w.put(diff if diff > 0 else diff + (1 << size) - 1, size)
                    w.code("10")             # end of block
    w.start(0xB7)                            # sequence end
    return bytes(w.out)


def write_movie_pack(mod):
    """The movie example (tools/mods/examples/movie_pack)."""
    width, height, pictures = 256, 144, 72

    def block(k, plane, bx, by):
        if plane == 0:
            bar = (k * 2) % (width // 8)     # a bright bar two blocks a picture, left to right
            if abs(bx - bar) <= 1:
                return 220
            return 40 + bx * 3 + by * 2
        return 128 + (24 if plane == 1 else -24) * math.sin(2 * math.pi * (k / pictures + by / (height // 16)))

    movie = mod / "movie"
    movie.mkdir(parents=True, exist_ok=True)
    (movie / "intro_wide.m2v").write_bytes(mpeg2_dc_video(width, height, 3, 1, pictures, block))
    rate = 22050
    frames = pictures * 1001 * rate // 24000
    write_wav(movie / "intro.wav", rate,
              [0.3 * min(1.0, n / 2000, (frames - n) / 2000) *
               (math.sin(2 * math.pi * 220 * n / rate) + 0.6 * math.sin(2 * math.pi * 277.18 * n / rate) +
                0.4 * math.sin(2 * math.pi * 329.63 * n / rate)) / 2 for n in range(frames)])
    (movie / "intro.sub").write_bytes(
        b"%FEED ENGLISH\n00:00:20 00:02:80 PRINT CENTER\n\"EXAMPLE MOVIE OVERRIDE\"\n\"(132md14 movie_pack)\"\n%END\n")
    manifest = "\n".join([
        "# The movie example (tools/make_example_mod.py --movie-pack; research/MODDING.md \"Movie overrides\"): all",
        "# synthetic. Copy this folder into mods\\ (or start omega_play with --mods tools\\mods\\examples); check it with",
        "# omega_mod_tool validate; pack it with omega_mod_tool pack [--platform vita].",
        "name=Example movie",
        "version=1.0",
        "author=make_example_mod.py",
        "description=A 3 s 16:9 stand-in for the intro movie, with its own sound and subtitles",
        "game_version=SCUS-97264",
        "priority=5",
        "enabled=1",
        "# <movie>=<file>[+<file>...]: a ZMOVIES.HOG movie (.PSS may be left out) and its replacement picture (.m2v /",
        "# .mpv MPEG-2 video, or a .pss), sound (.wav / .ogg) and subtitles (.sub), in any order, any of them. The key",
        "# may repeat; the items accumulate.",
        "movie_overrides=INTRO=movie/intro_wide.m2v+movie/intro.wav+movie/intro.sub",
        "notes.vita=The picture is 256x144 (within the Vita's 960x544 screen); it packs as it is.",
        "",
    ])
    (mod / "manifest.txt").write_text(manifest, encoding="utf-8", newline="\n")


def write_data_pack(mod, enabled=False):
    """The gameplay data example (tools/mods/examples/stronger_silenced_pistol)."""
    mod.mkdir(parents=True, exist_ok=True)
    manifest = "\n".join([
        "# The gameplay data example (research/MODDING.md \"Gameplay data overrides\"): a stronger silenced pistol.",
        "# A gameplay mod, so it ships disabled: copy this folder into mods\\ and enable it on the Mods page of the",
        "# port's options (or set enabled=1). Check it with omega_mod_tool validate; LAN and online games then need",
        "# the same gameplay mods on every side (the gameplay policy; cosmetic mods may differ).",
        "name=Stronger silenced pistol",
        "version=1.0",
        "author=132md16",
        "description=The silenced pistol (WEAPONS.WDB record 112, SocomSilenced) kills with one hit and holds 15 rounds",
        "game_version=SCUS-97264",
        "priority=5",
        "enabled=" + ("1" if enabled else "0"),
        "# INI files of tuning-table fields by name: [weapon <id|display key>], [npc <LEVEL> <id|player|*>], [npc * *],",
        "# [tohit]. Values are range-checked; omega_mod_tool validate lists every change against the disc.",
        "data_overrides=pistol.ini",
        "notes.pc=Gameplay: LAN and online games refuse a player whose gameplay mods differ from the host's.",
        "notes.vita=Plain text; packs as it is.",
        "",
    ])
    (mod / "manifest.txt").write_text(manifest, encoding="utf-8", newline="\n")
    (mod / "pistol.ini").write_text("\n".join([
        "# The silenced pistol (the hip slot's 112 in --loadout; the game's SocomSilenced). On the disc: damage 80 in",
        "# every band (a Toronto guard takes two hits) and a 12-round clip.",
        "[weapon SocomSilenced]",
        "damage_near = 150",
        "damage_mid = 150",
        "damage_far = 150",
        "clip = 15",
        "",
    ]), encoding="utf-8", newline="\n")


def write_beep(path, rate=22050, seconds=0.2, frequency=990.0):
    """A 16-bit mono WAV: a sine beep with an exponential decay."""
    frames = int(rate * seconds)
    data = b"".join(struct.pack("<h", int(30000 * math.exp(-12 * n / rate) * math.sin(2 * math.pi * frequency * n / rate)))
                    for n in range(frames))
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=str(REPO / "local" / "example_mods"), help="the mods folder to write into")
    parser.add_argument("--name", default="md6_example", help="the mod's folder")
    parser.add_argument("--priority", type=int, default=5)
    parser.add_argument("--disabled", action="store_true", help="write enabled=0 (enable it with omega_mod_tool enable)")
    parser.add_argument("--hd-pack", action="store_true",
                        help="132md12: write the HD texture pack example instead (default: tools/mods/examples/hd_pack)")
    parser.add_argument("--audio-pack", action="store_true",
                        help="132md13: write the audio example instead (default: tools/mods/examples/audio_pack)")
    parser.add_argument("--movie-pack", action="store_true",
                        help="132md14: write the movie example instead (default: tools/mods/examples/movie_pack)")
    parser.add_argument("--data-pack", action="store_true",
                        help="132md16: write the gameplay data example instead (default: "
                             "tools/mods/examples/stronger_silenced_pistol)")
    parser.add_argument("--enabled", action="store_true", help="132md16: --data-pack writes enabled=1 (a play test's copy)")
    args = parser.parse_args()
    if args.data_pack:
        out = Path(args.out) if "--out" in sys.argv else REPO / "tools" / "mods" / "examples"
        mod = out / (args.name if "--name" in sys.argv else "stronger_silenced_pistol")
        write_data_pack(mod, args.enabled)
        files = sorted(p for p in mod.rglob("*") if p.is_file())
        for p in files:
            print(f"{p.relative_to(mod)}  {p.stat().st_size} bytes")
        print(f"Wrote the gameplay data example {mod} ({len(files)} files)")
        return 0
    if args.movie_pack:
        out = Path(args.out) if "--out" in sys.argv else REPO / "tools" / "mods" / "examples"
        mod = out / (args.name if "--name" in sys.argv else "movie_pack")
        write_movie_pack(mod)
        files = sorted(p for p in mod.rglob("*") if p.is_file())
        for p in files:
            print(f"{p.relative_to(mod)}  {p.stat().st_size} bytes")
        print(f"Wrote the movie example {mod} ({len(files)} files)")
        return 0
    if args.audio_pack:
        out = Path(args.out) if "--out" in sys.argv else REPO / "tools" / "mods" / "examples"
        mod = out / (args.name if "--name" in sys.argv else "audio_pack")
        write_audio_pack(mod)
        files = sorted(p for p in mod.rglob("*") if p.is_file())
        for p in files:
            print(f"{p.relative_to(mod)}  {p.stat().st_size} bytes")
        print(f"Wrote the audio example {mod} ({len(files)} files)")
        return 0
    if args.hd_pack:
        out = Path(args.out) if "--out" in sys.argv else REPO / "tools" / "mods" / "examples"
        name = args.name if "--name" in sys.argv else "hd_pack"
        mod = out / name
        write_hd_pack(mod)
        files = sorted(p for p in mod.rglob("*") if p.is_file())
        for p in files:
            print(f"{p.relative_to(mod)}  {p.stat().st_size} bytes")
        print(f"Wrote the HD texture pack example {mod} ({len(files)} files)")
        return 0
    if not args.name or any(c in args.name for c in "\\/:*?\"<>|") or args.name in (".", ".."):
        sys.exit(f"--name {args.name!r} is not a folder name")

    mod = Path(args.out) / args.name
    write_png(mod / "textures" / "GAMEDATA" / "TORONTO1" / "TEX.HOG" / "WHITECONC.TDX.png", 256, 256, bricks)
    write_png(mod / "textures" / "GAMEDATA" / "COMMON" / "LOADING.HOG" / "BAR.TDX.png", 256, 256, bar)
    write_beep(mod / "sounds" / "GAMEDATA" / "TORONTO1" / "SND.HOG" / "WEPS.BNK" / "13.wav")
    manifest = "\n".join([
        "# An example mod (tools/make_example_mod.py, research/MODDING.md \"Mod author guide\"): all synthetic.",
        f"name=Example pack",
        "version=1.0",
        "author=make_example_mod.py",
        "description=4x bricks on Toronto's white concrete, a gradient loading bar, a beep for the 92F",
        f"priority={args.priority}",
        f"enabled={0 if args.disabled else 1}",
        "overrides=GAMEDATA/TORONTO1/TEX.HOG,GAMEDATA/COMMON/LOADING.HOG,GAMEDATA/TORONTO1/SND.HOG",
        "",
    ])
    (mod / "manifest.txt").write_text(manifest, encoding="utf-8")
    files = sorted(p for p in mod.rglob("*") if p.is_file())
    for p in files:
        print(f"{p.relative_to(mod)}  {p.stat().st_size} bytes")
    print(f"Wrote the example mod {mod} ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
