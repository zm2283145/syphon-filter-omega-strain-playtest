"""Blender round trip of the port's model exports (port-only tooling, checkpoint 132md3; research/MODDING.md
"Editing in Blender").

Exports models from the user's disc image with omega_mod_tool, re-saves every one through Blender's glTF importer and
exporter (Blender run headless: `blender --background`), then loads both sets as the play session does
(`omega_mod_tool check-models`) and compares them:

  python tools/blender_round_trip.py [--blender EXE] [--iso FILE] [--out DIR] [--tool EXE] [--subdivide N] [WHAT[@MATCH] ...]

WHAT is anything `omega_mod_tool export-model` takes (a character .SKL, a part, a level .VUM / .HOG, a gun archive);
@MATCH is its --match. The default set: the male agent (TORONTO1 PC_MALE.SKL, its face and hands with their shapes),
Gabe (UKRAINE_GABE.SKL), Lian (TOKYO LIAN_COMBAT.SKL, FEMALE.SKEL), every TORONTO1 gun (WPN.HOG), and five TORONTO1
level models: A07 (colour patch sets), A26 (an animated material), A103 / A123 (the regression start's street) and
the LGT_REDWARN_GLOW object.

Output (all disc-derived: keep it out of the repository; the default is under local\\, which git ignores):
  <out>\\export\\rt_export\\        the tool's export (models\\<key>.gltf), enabled as a mod
  <out>\\blender\\rt_blender\\      Blender's re-export of each (models\\<key>.glb), enabled as a mod
  <out>\\round_trip.txt            the comparison
Use either folder with the play session: `--mods <out>\\blender`.

Blender's settings (the ones a modder should use, research/MODDING.md):
  import: the defaults (bone direction "Blender", guess the original bind pose, shading from normals);
  export: glTF Binary, "Include All Bone Influences" (export_all_influences: the game's parts use up to 8),
          shape keys (with their normals; sparse, as Blender writes by default), vertex colours from the active
          colour attribute (export_vertex_color ACTIVE: a level model's COLOR_0), no animation.

Compared per model: accepted in both; the same triangle count, image size, morph targets (count and the shapes
they bind), colour patch sets (count) and animated surfaces; the bounds of every POSITION within 1e-4 of the
model's extent (plus 1e-5). Vertex counts are reported (Blender may split or merge vertices). A triangle the export
holds twice on the same vertices in the same winding (the disc draws it twice, a strip artefact) is kept once by
Blender; those are counted from the export and allowed. One repeated in the other winding (a two-sided pair) would
lose a side and differs. Exit 1 on a difference.

--subdivide N (checkpoint 132md6, research/MODDING.md "Mod author guide"): an HD edit instead of a plain re-save.
Blender cuts every edge of every mesh N times over (bmesh subdivide_edges, one cut, grid fill, no smoothing: each
triangle becomes 4^N), interpolating the skin weights, the shape keys, the UVs and the vertex colours, then exports as
above. The comparison then expects (the export's triangles - those drawn twice) x 4^N triangles and the same bounds,
images, morph targets, patch sets, animated surfaces and skeleton.

Inside Blender (internal): blender --background --factory-startup --python blender_round_trip.py -- --convert LIST
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_BLENDER = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
DEFAULT_WHAT = [
    "GAMEDATA/TORONTO1/SKL.HOG/PC_MALE.SKL",
    "GAMEDATA/UKRAINE/SKL.HOG/UKRAINE_GABE.SKL",
    "GAMEDATA/TOKYO/SKL.HOG/LIAN_COMBAT.SKL",
    "GAMEDATA/TORONTO1/WPN.HOG",
    "GAMEDATA/TORONTO1/DATA.HOG@TORONTO_DOWNTOWN_A07.",
    "GAMEDATA/TORONTO1/DATA.HOG@TORONTO_DOWNTOWN_A26.",
    "GAMEDATA/TORONTO1/DATA.HOG@TORONTO_DOWNTOWN_A103.",
    "GAMEDATA/TORONTO1/DATA.HOG@TORONTO_DOWNTOWN_A123.",
    "GAMEDATA/TORONTO1/OBJECTS.HOG@LGT_REDWARN_GLOW",
]
MANIFEST = "name={name}\nversion=0.1\nauthor=blender_round_trip.py\ndescription={description}\npriority={priority}\nenabled=1\n"


# ---- inside Blender ----

def subdivide(bpy, levels):
    import bmesh  # noqa: only inside Blender

    for obj in bpy.data.objects:
        if obj.type != "MESH":
            continue
        mesh = obj.data
        # The parts' normals are authored (the scalp's point up and out for the game's lighting), not the faces'
        # normals Blender would recompute: carry each vertex's imported normal through the subdivision.
        corner = mesh.corner_normals if hasattr(mesh, "corner_normals") else None
        sums = [[0.0, 0.0, 0.0] for _ in mesh.vertices]
        for loop in mesh.loops:
            n = corner[loop.index].vector if corner is not None else loop.normal
            for c in range(3):
                sums[loop.vertex_index][c] += n[c]
        bm = bmesh.new()
        bm.from_mesh(mesh)
        layer = bm.verts.layers.float_vector.new("round_trip_normal")
        bm.verts.ensure_lookup_table()
        for v in bm.verts:
            v[layer] = sums[v.index]
        for _ in range(levels):
            bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=1, use_grid_fill=True)
        bmesh.ops.triangulate(bm, faces=bm.faces[:])
        bm.verts.ensure_lookup_table()
        normals = []
        for v in bm.verts:
            n = v[layer]
            length = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
            normals.append((n[0] / length, n[1] / length, n[2] / length) if length > 1e-12 else tuple(v.normal))
        bm.verts.layers.float_vector.remove(layer)
        bm.to_mesh(mesh)
        bm.free()
        mesh.normals_split_custom_set_from_vertices(normals)
        mesh.update()


def blender_convert(list_file, levels=0):
    import bpy  # noqa: only inside Blender

    export_options = dict(export_format="GLB", export_animations=False, export_all_influences=True,
                          export_vertex_color="ACTIVE", export_morph=True, export_morph_normal=True, export_skins=True,
                          export_yup=True, export_apply=False)
    known = {p.identifier for p in bpy.ops.export_scene.gltf.get_rna_type().properties}
    export_options = {k: v for k, v in export_options.items() if k in known}
    pairs = [line.rstrip("\n").split("\t") for line in open(list_file, encoding="utf-8") if line.strip()]
    failed = 0
    for n, (src, dst) in enumerate(pairs, 1):
        start = time.time()
        try:
            bpy.ops.wm.read_factory_settings(use_empty=True)
            bpy.ops.import_scene.gltf(filepath=src)
            if levels:
                subdivide(bpy, levels)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            bpy.ops.export_scene.gltf(filepath=dst, **export_options)
            if not os.path.exists(dst):
                raise RuntimeError("no file written")
            print(f"RT {n}/{len(pairs)} ok {time.time() - start:.2f}s {dst}", flush=True)
        except Exception as error:  # report and go on: the driver compares what was written
            failed += 1
            print(f"RT {n}/{len(pairs)} FAILED {src}: {error}", flush=True)
    print(f"RT done: {len(pairs) - failed} of {len(pairs)} re-exported", flush=True)


# ---- the driver ----

def run(cmd, log=None):
    result = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if log is not None:
        log.append("$ " + " ".join(str(c) for c in cmd))
        log.append(result.stdout.rstrip())
        if result.stderr.strip():
            log.append(result.stderr.rstrip())
    return result


def gltf_json(path):
    data = path.read_bytes()
    if data[:4] == b"glTF":
        length = struct.unpack_from("<I", data, 12)[0]
        return json.loads(data[20:20 + length].decode("utf-8"))
    return json.loads(data.decode("utf-8-sig"))


def accessor_values(doc, path, index):
    """A non-sparse accessor's elements (tuples) of a .gltf with file buffers."""
    a = doc["accessors"][index]
    view = doc["bufferViews"][a["bufferView"]]
    data = (path.parent / doc["buffers"][view["buffer"]]["uri"]).read_bytes()
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    fmt = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[a["componentType"]]
    size = struct.calcsize(fmt) * n
    stride = view.get("byteStride", size)
    at = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    return [struct.unpack_from("<" + fmt * n, data, at + i * stride) for i in range(a["count"])]


def repeated_triangles(path):
    """The export's (.gltf) triangles repeating an earlier one's vertices: (same winding, other winding)."""
    doc = gltf_json(path)
    same = other = 0
    for mesh in doc.get("meshes", []):
        for prim in mesh.get("primitives", []):
            if "indices" not in prim or prim.get("mode", 4) != 4:
                continue
            idx = [v[0] for v in accessor_values(doc, path, prim["indices"])]
            seen = {}
            for k in range(0, len(idx) - 2, 3):
                tri = tuple(idx[k:k + 3])
                key = frozenset(tri)
                if len(key) < 3:
                    continue
                if key in seen:
                    first = seen[key]
                    rotations = {first, first[1:] + first[:1], first[2:] + first[:2]}
                    if tri in rotations:
                        same += 1
                    else:
                        other += 1
                else:
                    seen[key] = tri
    return same, other


def bounds(path):
    """Every POSITION accessor's min / max (glTF space), combined: (lo, hi) or None."""
    doc = gltf_json(path)
    accessors = doc.get("accessors", [])
    lo, hi = [float("inf")] * 3, [float("-inf")] * 3
    for mesh in doc.get("meshes", []):
        for prim in mesh.get("primitives", []):
            a = prim.get("attributes", {}).get("POSITION")
            if a is None or "min" not in accessors[a]:
                continue
            for k in range(3):
                lo[k] = min(lo[k], accessors[a]["min"][k])
                hi[k] = max(hi[k], accessors[a]["max"][k])
    return (lo, hi) if lo[0] != float("inf") else None


LINE = re.compile(r"^models/(\S+) (?:<- (\S+) \((.*)\)|from (\S+) REFUSED \((.*)\); the disc's model would be drawn)$")


def check(tool, iso, mods, log):
    cmd = [str(tool), "check-models", "--mods", str(mods)] + (["--iso", str(iso)] if iso else [])
    result = run(cmd, log)
    models = {}
    for line in result.stdout.splitlines():
        m = LINE.match(line.strip())
        if m:
            models[m.group(1)] = ("ok", m.group(3)) if m.group(2) else ("refused", m.group(5))
    return models


def numbers(what):
    """The comparable facts of a check-models summary."""
    facts = {}
    for name, pattern in (("vertices", r"(\d+) vertices"), ("triangles", r"(\d+) triangles"), ("image", r"image (\d+x\d+)"),
                          ("images", r"(\d+) images"), ("materials", r"(\d+) materials"),
                          ("morphs", r"(\d+) morph targets on (\d+) of (\d+) shapes"),
                          ("patch sets", r"(\d+) colour patch sets"), ("patched", r"on (\d+) vertices"),
                          ("animated", r"(\d+) animated surfaces kept"), ("skeleton", r"^([A-Z_0-9.]+SKEL|[A-Z_0-9.]+\.HOG's bones)")):
        m = re.search(pattern, what)
        facts[name] = m.groups() if m and len(m.groups()) > 1 else (m.group(1) if m else None)
    return facts


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("what", nargs="*", help="export-model arguments, optionally @MATCH (default: see the docstring)")
    parser.add_argument("--blender", default=os.environ.get("BLENDER", DEFAULT_BLENDER))
    parser.add_argument("--iso")
    parser.add_argument("--out", default=str(REPO / "local" / "blender_rt"))
    parser.add_argument("--tool", default=str(REPO / "local" / "build-native" / "Release" / "omega_mod_tool.exe"))
    parser.add_argument("--keep", action="store_true", help="keep an existing export (re-run Blender and the checks only)")
    parser.add_argument("--subdivide", type=int, default=0, metavar="N",
                        help="an HD edit: Blender subdivides every mesh N times (each triangle becomes 4^N)")
    parser.add_argument("--convert", help=argparse.SUPPRESS)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:])
    if args.convert:
        blender_convert(args.convert, args.subdivide)
        return 0

    out, tool = Path(args.out).resolve(), Path(args.tool)
    if not Path(args.blender).exists():
        sys.exit(f"Blender not found at {args.blender} (give --blender or set BLENDER); research/MODDING.md lists the steps by hand")
    if not tool.exists():
        sys.exit(f"{tool} not found: build the port first")
    export_mods, blender_mods = out / "export", out / "blender"
    export_mod, blender_mod = export_mods / "rt_export", blender_mods / "rt_blender"
    log, overwritten = [], []
    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)
        exported = {}
        for what in args.what or DEFAULT_WHAT:
            path, _, match = what.partition("@")
            cmd = [str(tool), "export-model", "--out", str(export_mod), path] + (["--match", match] if match else [])
            if args.iso:
                cmd += ["--iso", args.iso]
            result = run(cmd, log)
            if result.returncode:
                sys.exit(f"export of {what} failed:\n{result.stdout}\n{result.stderr}")
            # One model key (a COMMON part) exports with each level's texture of its name: a later level's export
            # replaces an earlier one's, and the mod then draws that look in every level.
            for f in sorted((export_mod / "models").rglob("*.gltf")) if (export_mod / "models").exists() else []:
                data = f.read_bytes() + (f.with_suffix(".png").read_bytes() if f.with_suffix(".png").exists() else b"")
                key = f.relative_to(export_mod / "models").as_posix()
                if key in exported and exported[key][1] != data:
                    overwritten.append(f"{key}: {exported[key][0]}'s export replaced by {what}'s")
                exported[key] = (exported[key][0] if key in exported and exported[key][1] == data else what, data)
        for line in overwritten:
            print(f"Note: {line}", flush=True)
    shutil.rmtree(blender_mods, ignore_errors=True)
    (export_mod / "manifest.txt").write_text(MANIFEST.format(name="rt_export", description="the tool's export", priority=10))
    blender_mod.mkdir(parents=True)
    (blender_mod / "manifest.txt").write_text(MANIFEST.format(name="rt_blender", description="Blender's re-export", priority=10))

    sources = sorted((export_mod / "models").rglob("*.gltf"))
    pairs = []
    for src in sources:
        rel = src.relative_to(export_mod / "models")
        pairs.append((src, blender_mod / "models" / rel.with_suffix(".glb")))
    list_file = out / "convert_list.txt"
    list_file.write_text("".join(f"{s}\t{d}\n" for s, d in pairs), encoding="utf-8")
    start = time.time()
    result = run([args.blender, "--background", "--factory-startup", "--python", str(Path(__file__).resolve()), "--",
                  "--convert", str(list_file)] + (["--subdivide", str(args.subdivide)] if args.subdivide else []))
    blender_seconds = time.time() - start
    rt_lines = [line for line in result.stdout.splitlines() if line.startswith("RT ")]
    (out / "blender_log.txt").write_text(result.stdout + "\n" + result.stderr, encoding="utf-8")
    version = next((line.strip() for line in result.stdout.splitlines() if line.startswith("Blender ")), "Blender ?")

    original = check(tool, args.iso, export_mods, log)
    edited = check(tool, args.iso, blender_mods, log)
    report, problems = [], 0
    totals = {"models": 0, "vertices": [0, 0], "triangles": 0, "morph": 0, "patch": 0, "animated": 0, "merged": 0}
    worst = 0.0
    for src, dst in pairs:
        key = src.relative_to(export_mod / "models").with_suffix("").as_posix().upper()
        a, b = original.get(key), edited.get(key)
        issues = []
        if not dst.exists():
            issues.append("Blender wrote no file")
        if not a or a[0] != "ok":
            issues.append(f"the export itself: {a}")
        if not b or b[0] != "ok":
            issues.append(f"the round trip: {b[1] if b else 'not checked'}")
        line = key
        if a and b and a[0] == b[0] == "ok":
            fa, fb = numbers(a[1]), numbers(b[1])
            for name in ("image", "images", "materials", "morphs", "patch sets", "animated", "skeleton"):
                if fa[name] != fb[name]:
                    issues.append(f"{name} {fa[name]} -> {fb[name]}")
            kept_once = ""
            if args.subdivide:
                same, other = repeated_triangles(src)
                want = (int(fa["triangles"]) - same) * 4 ** args.subdivide
                if int(fb["triangles"]) != want:
                    issues.append(f"triangles {fa['triangles']} -> {fb['triangles']}, expected ({fa['triangles']} - {same} "
                                  f"drawn twice) x {4 ** args.subdivide} = {want}")
                kept_once = f" (subdivided {args.subdivide}x)" + (f", {same} drawn twice on the disc kept once" if same else "")
                totals["merged"] += same
            elif fa["triangles"] != fb["triangles"]:
                same, other = repeated_triangles(src)
                if int(fa["triangles"]) - int(fb["triangles"]) == same and not other:
                    kept_once = f" ({same} drawn twice on the disc, kept once)"
                    totals["merged"] += same
                else:
                    issues.append(f"triangles {fa['triangles']} -> {fb['triangles']} (the export repeats {same} in the "
                                  f"same winding, {other} in the other)")
            ba, bb = bounds(src), bounds(dst)
            if ba and bb:
                extent = max(hi - lo for lo, hi in zip(*ba))
                diff = max(max(abs(x - y) for x, y in zip(ba[0], bb[0])), max(abs(x - y) for x, y in zip(ba[1], bb[1])))
                worst = max(worst, diff / extent if extent else 0.0)
                if diff > 1e-4 * extent + 1e-5:
                    issues.append(f"bounds move {diff:.3g} (extent {extent:.3g})")
            elif bool(ba) != bool(bb):
                issues.append("bounds missing")
            line += f": vertices {fa['vertices']} -> {fb['vertices']}, triangles {fa['triangles']} -> {fb['triangles']}{kept_once}"
            for name in ("morphs", "patch sets", "animated", "image"):
                if fb[name]:
                    line += f", {name} {fb[name] if isinstance(fb[name], str) else '/'.join(fb[name])}"
            totals["models"] += not issues
            totals["vertices"][0] += int(fa["vertices"] or 0)
            totals["vertices"][1] += int(fb["vertices"] or 0)
            totals["triangles"] += int(fb["triangles"] or 0)
            totals["morph"] += int(fb["morphs"][0]) if fb["morphs"] else 0
            totals["patch"] += int(fb["patch sets"] or 0)
            totals["animated"] += int(fb["animated"] or 0)
        if issues:
            problems += 1
            line += "  DIFFERS: " + "; ".join(issues)
        report.append(line)
    edit = f", subdivided {args.subdivide}x" if args.subdivide else ""
    summary = (f"{version}: {len(pairs)} models re-exported{edit} in {blender_seconds:.0f} s; {totals['models']} identical in "
               f"what the game uses (in all: {totals['triangles']} triangles, {totals['morph']} morph targets, "
               f"{totals['patch']} colour patch sets, {totals['animated']} animated surfaces; vertices "
               f"{totals['vertices'][0]} -> {totals['vertices'][1]}; bounds within {worst:.2g} of the extent; "
               f"{totals['merged']} triangles drawn twice on the disc kept once); {problems} differ")
    if overwritten:
        summary += (f"; {len(overwritten)} export(s) replaced by a later level's (a shared part takes each level's "
                    "texture of its name: export one level per mod)")
    text = "\n".join([summary, ""] + report + ["", "Blender:"] + rt_lines[-3:] + ["", "Log:"] + log) + "\n"
    (out / "round_trip.txt").write_text(text, encoding="utf-8")
    print("\n".join(line for line in report if "DIFFERS" in line))
    print(summary)
    print(f"Report: {out / 'round_trip.txt'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
