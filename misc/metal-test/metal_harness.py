#!/usr/bin/env python3
"""
metal_harness.py - automated runtime test harness for the dhewm3 Metal renderer (macOS).

Adapted from the ioq3 Metal harness (aramfingal/ioq3 misc/metal-test). It drives real game
sessions through scripted console configs, then collects console logs, screenshots,
timedemo numbers and crash reports into a results folder with a Markdown report.

Subcommands
  build    --src DIR                      build a checkout with CMake + Ninja, print the binary
  run      --bin BIN --label NAME         run every scenario against one build / renderer
  compare  RESULTS_A RESULTS_B            diff two runs (images + metrics) into a report
  all      --bin BIN [--baseline-bin B]   run metal + gl (+ baseline) and compare, one command

See misc/metal-test/README.md. Python 3.9+, standard library only (numpy speeds up image
diffs when it is installed).
"""

import argparse
import datetime
import glob
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time
import zipfile
import zlib

try:
    import numpy as np  # optional, only speeds up image diffs
except ImportError:  # pragma: no cover
    np = None

DEFAULT_DATA = os.path.expanduser("~/Projects/doom-3-extracted")
DEFAULT_RES = (1280, 720)
SCENARIO_TIMEOUT = 300
EXE_NAME = "dhewm3"

# (fs_game, map, kind). kind "views": noclip to fixed view positions taken from the map's
# item entities. kind "intro": the map opens with a scripted camera; shots at fixed tics.
ALL_MAPS = [
    ("base", "game/mars_city1", "intro"),
    ("base", "game/mars_city1", "views"),
    ("base", "game/alphalabs1", "views"),
    ("base", "game/delta2a", "views"),
    ("base", "game/hell1", "views"),
    ("d3xp", "game/erebus1", "views"),
    ("d3xp", "game/hell", "views"),
]
QUICK_MAPS = [ALL_MAPS[1], ALL_MAPS[2], ALL_MAPS[4], ALL_MAPS[5]]
VIEWS_PER_MAP = 6
INTRO_SHOT_TICS = [700, 1000, 1400, 1800]

# Image comparison thresholds (applied on a half-resolution sample of each image).
PIXEL_DIFF_THRESHOLD = 32      # max channel difference that counts a pixel as "different"
FLAG_PERCENT = 0.5             # % of differing pixels that flags an image for review
TIMEDEMO_REGRESSION_PCT = 5.0  # fps drop that flags a performance regression

# Console lines that are expected noise and never count as renderer problems.
BENIGN_PATTERNS = [
    r"Couldn't load gui",
    r"WARNING: .*\.(map|proc|script|def|mtr|gui|af|skin|sndshd|fx|pda|md5\w*|lwo|ase|wav|ogg)\b",
    r"WARNING: (idCollisionModelManagerLocal|idSoundCache|idAFEntity|idAnimator|idDeclManager)",
    r"missing or invalid sound|no sound system|Sound",
    r"libbacktrace",
    r"Unknown command 'r_backend'",
    r"terminal support disabled",
    r"Metal (API|GPU) Validation Enabled",
    r"_home/",
]
PROBLEM_PATTERNS = [
    r"\[Metal\].*(fail|error|warn|unsupported|not implemented)",
    r"MSL compile failed",
    r"^ERROR:",
    r"^\*+\s*ERROR",
    r"Error:",
    r"validation",
    r"assert",
    r"-\[MTL",
    r"signal caught|Segmentation|Abort trap|Bus error",
    r"WARNING: .*(image|glprog|program|shader|material|texture)",
]

BACKGROUND = True   # the game never comes to the front (--foreground turns this off)
EXTRA_CVARS = {}     # --set NAME VALUE: cvars added to every launch


def log(msg):
    print("[harness] " + msg, flush=True)


def write_json(path, data):
    with open(path, "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)


def read_json(path):
    with open(path) as f:
        return json.load(f)


def git_describe(path):
    try:
        out = subprocess.run(["git", "-C", path, "log", "-1", "--format=%h %s"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip()
    except Exception:
        return ""


def find_binary(path):
    """Accept a dhewm3 binary, an .app bundle or a build directory."""
    path = os.path.abspath(os.path.expanduser(path))
    candidates = [
        path,
        os.path.join(path, "Contents", "MacOS", EXE_NAME),
        os.path.join(path, EXE_NAME + ".app", "Contents", "MacOS", EXE_NAME),
        os.path.join(path, "build", EXE_NAME + ".app", "Contents", "MacOS", EXE_NAME),
        os.path.join(path, EXE_NAME),
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    sys.exit("error: no dhewm3 binary found at %s" % path)


# ----------------------------------------------------------------------------------
# image helpers (TGA in, PNG out, diff stats)
# ----------------------------------------------------------------------------------

def read_tga(path):
    """Read an uncompressed 24/32-bit TGA. Returns (width, height, rgb bytes top-down)."""
    with open(path, "rb") as f:
        data = f.read()
    id_len, cmap_type, img_type = data[0], data[1], data[2]
    if img_type not in (2, 10) or cmap_type != 0:
        raise ValueError("unsupported TGA type %d in %s" % (img_type, path))
    width, height = struct.unpack("<HH", data[12:16])
    bpp, desc = data[16], data[17]
    bytes_pp = bpp // 8
    pixels = data[18 + id_len:]
    if img_type == 10:  # run-length encoded (dhewm3 writes these)
        out = bytearray()
        total = width * height * bytes_pp
        i = 0
        while len(out) < total:
            head = pixels[i]
            n = (head & 0x7F) + 1
            i += 1
            if head & 0x80:
                out += pixels[i:i + bytes_pp] * n
                i += bytes_pp
            else:
                out += pixels[i:i + n * bytes_pp]
                i += n * bytes_pp
        pixels = bytes(out)
    stride = width * bytes_pp
    top_down = bool(desc & 0x20)
    rows = []
    for y in range(height):
        src_y = y if top_down else height - 1 - y
        row = pixels[src_y * stride:(src_y + 1) * stride]
        rgb = bytearray(width * 3)
        rgb[0::3] = row[2::bytes_pp]
        rgb[1::3] = row[1::bytes_pp]
        rgb[2::3] = row[0::bytes_pp]
        rows.append(bytes(rgb))
    return width, height, b"".join(rows)


def write_png(path, width, height, rgb):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def downsample(width, height, rgb, factor):
    if factor <= 1:
        return width, height, rgb
    w2, h2 = width // factor, height // factor
    stride = width * 3
    out = bytearray()
    for y in range(h2):
        row = rgb[y * factor * stride:(y * factor + 1) * stride]
        for x in range(w2):
            o = x * factor * 3
            out += row[o:o + 3]
    return w2, h2, bytes(out)


def tga_to_png(tga, png, factor=1):
    w, h, rgb = read_tga(tga)
    w, h, rgb = downsample(w, h, rgb, factor)
    write_png(png, w, h, rgb)


def image_stats(path):
    """Mean brightness and share of near-black pixels: catches all-black / blank frames."""
    w, h, rgb = read_tga(path)
    w, h, rgb = downsample(w, h, rgb, 4)
    n = len(rgb)
    mean = sum(rgb) / float(n) if n else 0.0
    dark = sum(1 for i in range(0, n, 3) if rgb[i] < 8 and rgb[i + 1] < 8 and rgb[i + 2] < 8)
    return {"mean": round(mean, 2), "pct_black": round(100.0 * dark / (n / 3), 1)}


def diff_images(path_a, path_b, triptych_png=None):
    """Compare two TGAs. Returns stats dict; optionally writes A | B | diff (half res)."""
    wa, ha, a = read_tga(path_a)
    wb, hb, b = read_tga(path_b)
    if (wa, ha) != (wb, hb):
        return {"error": "size mismatch %dx%d vs %dx%d" % (wa, ha, wb, hb)}
    w, h, a = downsample(wa, ha, a, 2)
    _, _, b = downsample(wb, hb, b, 2)
    n = w * h
    if np is not None:
        aa = np.frombuffer(a, dtype=np.uint8).reshape(h, w, 3).astype(np.int16)
        bb = np.frombuffer(b, dtype=np.uint8).reshape(h, w, 3).astype(np.int16)
        d = np.abs(aa - bb).max(axis=2)
        differing = int((d > PIXEL_DIFF_THRESHOLD).sum())
        mean = float(d.mean())
        dmax = int(d.max())
        dmap = np.clip(d * 4, 0, 255).astype(np.uint8).tobytes()
    else:
        dvals = bytearray(n)
        differing = 0
        total = 0
        dmax = 0
        for i in range(n):
            o = i * 3
            v = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
            total += v
            if v > dmax:
                dmax = v
            if v > PIXEL_DIFF_THRESHOLD:
                differing += 1
            dvals[i] = min(255, v * 4)
        mean = total / float(n)
        dmap = bytes(dvals)
    stats = {
        "mean_abs_diff": round(mean, 3),
        "max_diff": dmax,
        "pct_pixels_differ": round(100.0 * differing / n, 3),
    }
    stats["flagged"] = stats["pct_pixels_differ"] > FLAG_PERCENT
    if triptych_png:
        stride = w * 3
        out = bytearray()
        for y in range(h):
            out += a[y * stride:(y + 1) * stride]
            out += b[y * stride:(y + 1) * stride]
            drow = dmap[y * w:(y + 1) * w]
            red = bytearray(w * 3)
            red[0::3] = drow
            out += red
        write_png(triptych_png, w * 3, h, bytes(out))
    return stats


# ----------------------------------------------------------------------------------
# console log parsing
# ----------------------------------------------------------------------------------

def parse_log(text):
    problems = []
    for line in text.splitlines():
        clean = re.sub(r"\^\d", "", line).strip()
        if not clean:
            continue
        if any(re.search(p, clean) for p in BENIGN_PATTERNS):
            continue
        if any(re.search(p, clean, re.IGNORECASE) for p in PROBLEM_PATTERNS):
            if clean not in problems:
                problems.append(clean)
    metrics = {}
    m = re.search(r"(\d+) frames rendered in\s+([\d.]+) seconds =\s+([\d.]+) fps", text)
    if m:
        metrics["timedemo"] = {"frames": int(m.group(1)), "seconds": float(m.group(2)),
                               "fps": float(m.group(3))}
    backend = [float(x) for x in re.findall(r"frame stats \(\d+ frames[^)]*\):.*?backend ([\d.]+) ms", text)]
    if len(backend) > 2:
        metrics["backend_ms"] = round(statistics.median(backend), 4)
    dev = re.search(r"\[Metal\] device: (.*)", text)
    if dev:
        metrics["device"] = dev.group(1).strip()
    return problems, metrics


# ----------------------------------------------------------------------------------
# view positions and console scripts
# ----------------------------------------------------------------------------------

_viewpoint_cache = {}


def map_viewpoints(data, game, mapname, count=VIEWS_PER_MAP):
    """Fixed view positions for a map: `count` item entities spread through the map file,
    eye 56 units above each, looking along a yaw that cycles in 90 degree steps. The same
    data always gives the same views, in every build."""
    key = (data, game, mapname)
    if key in _viewpoint_cache:
        return _viewpoint_cache[key]
    text = None
    for folder in ([game, "base"] if game != "base" else ["base"]):
        for pk in sorted(glob.glob(os.path.join(data, folder, "*.pk4")), reverse=True):
            try:
                with zipfile.ZipFile(pk) as z:
                    name = "maps/%s.map" % mapname
                    if name in z.namelist():
                        text = z.read(name).decode("latin1")
                        break
            except zipfile.BadZipFile:
                continue
        if text:
            break
    if not text:
        sys.exit("error: maps/%s.map not found under %s" % (mapname, data))
    points = []
    for ent in re.findall(r"\{\s*\n((?:\"[^\n]*\n)+)", text):
        cls = re.search(r'"classname"\s+"([^"]+)"', ent)
        org = re.search(r'"origin"\s+"([-\d.]+) ([-\d.]+) ([-\d.]+)"', ent)
        if cls and org and re.match(r"(item_|ammo_|weapon_|info_player_start)", cls.group(1)):
            points.append(tuple(float(org.group(i)) for i in (1, 2, 3)))
    if not points:
        sys.exit("error: no view positions found in %s" % mapname)
    step = max(1, len(points) // count)
    views = []
    for i in range(count):
        x, y, z = points[min(len(points) - 1, i * step)]
        views.append((round(x, 1), round(y, 1), round(z + 56, 1), (i * 90 + 45) % 360))
    _viewpoint_cache[key] = views
    return views


def views_cfg(data, game, mapname):
    """Fixed views of a frozen world: skip the opening cinematic if there is one, let the
    level run for a fixed number of tics, then stop game time (g_stopTime) so lights,
    animations and particles hold still while the camera moves."""
    lines = ["map %s" % mapname]
    for _ in range(10):  # an opening cinematic can start several seconds into the level
        lines += ["wait 60", "skipCinematic"]
    lines += ["wait 150", "god", "notarget", "noclip", "wait 2", "set g_stopTime 1", "wait 2"]
    for i, (x, y, z, yaw) in enumerate(map_viewpoints(data, game, mapname)):
        lines += ["setviewpos %s %s %s %s" % (x, y, z, yaw), "wait 10",
                  "screenshot shots/view%d.tga" % i, "wait 3"]
    lines += ["quit"]
    return "\n".join(lines) + "\n"


def intro_cfg(mapname):
    lines = ["map %s" % mapname]
    last = 0
    for i, tic in enumerate(INTRO_SHOT_TICS):
        lines += ["wait %d" % (tic - last), "screenshot shots/intro%d.tga" % i, "wait 3"]
        last = tic + 3
    lines += ["quit"]
    return "\n".join(lines) + "\n"


def menu_cfg():
    # startup commands suppress the main menu; "disconnect" brings it up
    return "\n".join(["wait 60", "disconnect", "wait 300", "screenshot shots/menu0.tga", "wait 100",
                      "screenshot shots/menu1.tga", "wait 3", "quit"]) + "\n"


def restart_cfg(mapname):
    """vid_restart with a map loaded: every image and program is purged and reloaded."""
    return "\n".join(["map %s" % mapname, "wait 90", "screenshot shots/before.tga", "wait 3",
                      "vid_restart", "wait 90", "screenshot shots/after.tga", "wait 3",
                      "quit"]) + "\n"


def timedemo_cfg(demo):
    return "timeDemoQuit %s\n" % demo


def scen_name(prefix, game, mapname):
    return "%s_%s_%s" % (prefix, game, mapname.split("/")[-1])


def build_scenarios(args, renderer):
    maps = QUICK_MAPS if args.suite == "quick" else ALL_MAPS
    scen = [{"name": "menu", "game": "base", "cfg": menu_cfg(), "shots": ["menu0", "menu1"],
             "diff": False, "cvars": {"com_fixedTic": "1"}}]
    for game, mapname, kind in maps:
        if kind == "views":
            scen.append({"name": scen_name("views", game, mapname), "game": game,
                         "cfg": views_cfg(args.data, game, mapname),
                         "shots": ["view%d" % i for i in range(VIEWS_PER_MAP)],
                         "diff": True, "cvars": {"com_fixedTic": "1"}})
        else:
            scen.append({"name": scen_name("intro", game, mapname), "game": game,
                         "cfg": intro_cfg(mapname),
                         "shots": ["intro%d" % i for i in range(len(INTRO_SHOT_TICS))],
                         "diff": True, "cvars": {"com_fixedTic": "1"}})
    if args.suite == "full":
        scen.append({"name": "restart_base_alphalabs1", "game": "base",
                     "cfg": restart_cfg("game/alphalabs1"), "shots": ["before", "after"],
                     "diff": False, "cvars": {"com_fixedTic": "1"}})
    if not args.no_timedemo:
        for run in range(1, (2 if args.suite == "quick" else 3) + 1):
            scen.append({"name": "timedemo_demo1_r%d" % run, "game": "base",
                         "cfg": timedemo_cfg("demo1"), "shots": [], "diff": False,
                         "timedemo": True, "timeout": 900,
                         # com_fixedTic -1: frames are not held to the 60Hz game tic
                         "cvars": {"com_fixedTic": "-1", "r_swapInterval": "0"}})
    if args.validation and renderer == "metal":
        game, mapname, _ = QUICK_MAPS[1]
        scen.append({"name": scen_name("validation", game, mapname), "game": game,
                     "cfg": views_cfg(args.data, game, mapname),
                     "shots": ["view%d" % i for i in range(VIEWS_PER_MAP)], "diff": False,
                     "cvars": {"com_fixedTic": "1"}, "timeout": 600,
                     "env": {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1",
                             "MTL_DEBUG_LAYER_ERROR_MODE": "nslog"}})
    if args.only:
        rx = re.compile(args.only)
        scen = [s for s in scen if rx.search(s["name"])]
    return scen


# ----------------------------------------------------------------------------------
# launching
# ----------------------------------------------------------------------------------

def base_command(binary, renderer, data, home, res, game, cvars):
    cmd = [binary,
           "+set", "fs_basepath", data,
           "+set", "fs_savepath", home,
           "+set", "fs_configpath", home,
           "+set", "r_backend", "metal" if renderer == "metal" else "opengl",
           "+set", "r_fullscreen", "0",
           "+set", "r_mode", "-1",
           "+set", "r_customWidth", str(res[0]),
           "+set", "r_customHeight", str(res[1]),
           "+set", "r_multiSamples", "0",
           "+set", "r_swapInterval", "0",
           "+set", "r_gamma", "1",
           "+set", "r_brightness", "1",
           "+set", "sys_lang", "english",   # the game data's default.cfg may pick another
           "+set", "com_machineSpec", "3",
           "+set", "image_anisotropy", "1",
           "+set", "in_nograb", "1",
           "+set", "s_noSound", "1",
           "+set", "com_showFPS", "0",
           "+set", "g_showHud", "1",
           "+set", "developer", "0",
           "+set", "logFile", "2"]
    if game != "base":
        cmd += ["+set", "fs_game", game]
    for k, v in cvars.items():
        cmd += ["+set", k, v]
    return cmd


def new_crash_reports(since):
    found = []
    for d in [os.path.expanduser("~/Library/Logs/DiagnosticReports"),
              "/Library/Logs/DiagnosticReports"]:
        for p in glob.glob(os.path.join(d, EXE_NAME + "*")):
            try:
                if os.path.getmtime(p) >= since - 1:
                    found.append(p)
            except OSError:
                pass
    return found


def launch(binary, renderer, data, home, res, game, cvars, cfg_text, outdir, timeout, env=None):
    gamedir = os.path.join(home, game)
    os.makedirs(gamedir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    for d in (gamedir, outdir):
        with open(os.path.join(d, "harness.cfg"), "w") as f:
            f.write(cfg_text)
    if BACKGROUND and renderer == "metal":
        cvars = dict(cvars, r_metalHidden="1")
    if EXTRA_CVARS:
        cvars = dict(cvars, **EXTRA_CVARS)
    cmd = base_command(binary, renderer, data, home, res, game, cvars) + ["+exec", "harness.cfg"]
    with open(os.path.join(outdir, "command.txt"), "w") as f:
        f.write(" ".join("'%s'" % c if " " in c else c for c in cmd) + "\n")
    run_env = dict(os.environ)
    run_env.pop("MTL_HUD_ENABLED", None)
    if BACKGROUND:
        run_env["SDL_MAC_BACKGROUND_APP"] = "1"
    if env:
        run_env.update(env)
    start = time.time()
    timed_out = False
    with open(os.path.join(outdir, "stdout.txt"), "w") as out:
        proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=run_env, cwd=home)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    return {"exit_code": proc.returncode, "timed_out": timed_out,
            "duration_s": round(time.time() - start, 1), "started": start}


def run_scenario(args, label_dir, scen, binary, renderer):
    outdir = os.path.join(label_dir, scen["name"])
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    home = os.path.join(args.out, "_home", os.path.basename(label_dir) + "_" + scen["name"])
    if os.path.isdir(home):
        shutil.rmtree(home)
    os.makedirs(home)
    res = launch(binary, renderer, args.data, home, args.res, scen["game"], scen.get("cvars", {}),
                 scen["cfg"], outdir, scen.get("timeout", args.timeout), scen.get("env"))
    gamedir = os.path.join(home, scen["game"])
    reasons = []
    status = "PASS"
    if res["timed_out"]:
        status = "FAIL"
        reasons.append("timed out after %ss" % scen.get("timeout", args.timeout))
    elif res["exit_code"] != 0:
        status = "FAIL"
        reasons.append("exit code %s" % res["exit_code"])
    # console log: the game's own log if it was written, else process output
    console = ""
    qlog = os.path.join(gamedir, "qconsole.log")
    if os.path.isfile(qlog):
        shutil.copy(qlog, os.path.join(outdir, "qconsole.log"))
        with open(qlog, errors="replace") as f:
            console = f.read()
    with open(os.path.join(outdir, "stdout.txt"), errors="replace") as f:
        stdout = f.read()
    problems, metrics = parse_log(console + "\n" + stdout)
    crashes = new_crash_reports(res["started"])
    for c in crashes:
        try:
            shutil.copy(c, outdir)
        except OSError:
            pass
    if crashes:
        status = "FAIL"
        reasons.append("crash report: %s" % ", ".join(os.path.basename(c) for c in crashes))
    shots = {}
    shots_dir = os.path.join(outdir, "shots")
    os.makedirs(shots_dir, exist_ok=True)
    for shot in scen["shots"]:
        src = os.path.join(gamedir, "shots", shot + ".tga")
        if not os.path.isfile(src):
            status = "FAIL"
            reasons.append("missing screenshot %s" % shot)
            continue
        dst = os.path.join(shots_dir, shot + ".tga")
        shutil.copy(src, dst)
        try:
            tga_to_png(dst, os.path.join(shots_dir, shot + ".png"))
            st = image_stats(dst)
            shots[shot] = st
            if st["pct_black"] > 98.0:
                if status == "PASS":
                    status = "WARN"
                reasons.append("screenshot %s is blank (%.0f%% black)" % (shot, st["pct_black"]))
        except Exception as e:  # unreadable image
            status = "FAIL"
            reasons.append("bad screenshot %s: %s" % (shot, e))
    if scen.get("timedemo") and "timedemo" not in metrics:
        status = "FAIL"
        reasons.append("no timedemo result in the log")
    if problems and status == "PASS":
        status = "WARN"
    if problems:
        reasons.append("%d suspicious console line(s)" % len(problems))
    result = {"name": scen["name"], "status": status, "reasons": reasons, "problems": problems[:40],
              "metrics": metrics, "shots": shots, "diff": scen.get("diff", False),
              "exit_code": res["exit_code"], "duration_s": res["duration_s"], "renderer": renderer}
    write_json(os.path.join(outdir, "result.json"), result)
    if not args.keep_home:
        shutil.rmtree(home, ignore_errors=True)
    log("%-34s %-5s %5.1fs  %s" % (scen["name"], status, res["duration_s"], "; ".join(reasons)))
    return result


def run_build(args, label, binary, renderer):
    label_dir = os.path.join(args.out, label)
    os.makedirs(label_dir, exist_ok=True)
    log("=== %s: %s (%s) ===" % (label, binary, renderer))
    results = []
    for scen in build_scenarios(args, renderer):
        results.append(run_scenario(args, label_dir, scen, binary, renderer))
    # with --only, keep earlier results of scenarios that were not re-run
    merged = {}
    summary_path = os.path.join(label_dir, "summary.json")
    if args.only and os.path.isfile(summary_path):
        for r in read_json(summary_path).get("results", []):
            merged[r["name"]] = r
    for r in results:
        merged[r["name"]] = r
    summary = {"label": label, "binary": binary, "renderer": renderer,
               "git": git_describe(os.path.dirname(binary)),
               "date": datetime.datetime.now().isoformat(timespec="seconds"),
               "results": list(merged.values())}
    write_json(summary_path, summary)
    return label_dir


# ----------------------------------------------------------------------------------
# comparing runs, report
# ----------------------------------------------------------------------------------

def timedemo_fps(summary):
    vals = [r["metrics"]["timedemo"]["fps"] for r in summary["results"]
            if r["name"].startswith("timedemo_") and "timedemo" in r.get("metrics", {})]
    return max(vals) if vals else None


def compare_runs(dir_a, dir_b, out_dir, strict):
    """Image diffs of every shared screenshot. strict: flagged images are findings (same
    renderer, older build). Otherwise they are listed for review (Metal vs OpenGL)."""
    sa, sb = read_json(os.path.join(dir_a, "summary.json")), read_json(os.path.join(dir_b, "summary.json"))
    name_b = os.path.basename(dir_b)
    diffs_dir = os.path.join(out_dir, "diffs")
    os.makedirs(diffs_dir, exist_ok=True)
    rb = {r["name"]: r for r in sb["results"]}
    rows, findings = [], []
    for ra in sa["results"]:
        other = rb.get(ra["name"])
        if not other or not ra.get("diff"):
            continue
        for shot in sorted(ra["shots"]):
            pa = os.path.join(dir_a, ra["name"], "shots", shot + ".tga")
            pb = os.path.join(dir_b, ra["name"], "shots", shot + ".tga")
            if not (os.path.isfile(pa) and os.path.isfile(pb)):
                continue
            trip = os.path.join(diffs_dir, "%s__%s__%s.png" % (name_b, ra["name"], shot))
            st = diff_images(pa, pb, trip)
            st.update({"scenario": ra["name"], "shot": shot, "triptych": os.path.relpath(trip, out_dir)})
            rows.append(st)
            if st.get("error") or st.get("flagged"):
                findings.append({"severity": "FAIL" if strict else "REVIEW",
                                 "text": "%s/%s differs from %s: %s" % (
                                     ra["name"], shot, name_b,
                                     st.get("error") or "%.2f%% of pixels, mean %.2f" % (
                                         st["pct_pixels_differ"], st["mean_abs_diff"]))})
    fa, fb = timedemo_fps(sa), timedemo_fps(sb)
    perf = {"a_fps": fa, "b_fps": fb}
    if fa and fb and strict and fa < fb * (1 - TIMEDEMO_REGRESSION_PCT / 100.0):
        findings.append({"severity": "REVIEW",
                         "text": "timedemo %.1f fps vs %s %.1f fps" % (fa, name_b, fb)})
    return {"other": name_b, "rows": rows, "findings": findings, "perf": perf}


def write_report(out, candidate_dir, comparisons, extra_dirs):
    summary = read_json(os.path.join(candidate_dir, "summary.json"))
    findings = []
    for r in summary["results"]:
        if r["status"] == "FAIL":
            findings.append({"severity": "FAIL", "text": "%s: %s" % (r["name"], "; ".join(r["reasons"]))})
        elif r["status"] == "WARN":
            findings.append({"severity": "REVIEW", "text": "%s: %s" % (r["name"], "; ".join(r["reasons"]))})
    for d in extra_dirs:
        s = read_json(os.path.join(d, "summary.json"))
        for r in s["results"]:
            if r["status"] == "FAIL":
                findings.append({"severity": "REVIEW", "text": "reference build %s: %s: %s" % (
                    s["label"], r["name"], "; ".join(r["reasons"]))})
    for c in comparisons:
        findings += c["findings"]
    verdict = "FAIL" if any(f["severity"] == "FAIL" for f in findings) else \
        ("REVIEW" if findings else "PASS")
    lines = ["# dhewm3 Metal harness report", "",
             "Verdict: **%s**" % verdict, "",
             "- candidate: `%s` (%s), renderer %s" % (summary["binary"], summary["git"], summary["renderer"]),
             "- date: %s" % summary["date"], ""]
    lines += ["## Findings", ""]
    lines += ["- %s: %s" % (f["severity"], f["text"]) for f in findings] or ["None."]
    for d in [candidate_dir] + list(extra_dirs):
        s = read_json(os.path.join(d, "summary.json"))
        lines += ["", "## Scenarios: %s (%s)" % (s["label"], s["renderer"]), "",
                  "| scenario | status | seconds | notes |", "|---|---|---|---|"]
        for r in s["results"]:
            note = "; ".join(r["reasons"])
            td = r.get("metrics", {}).get("timedemo")
            if td:
                note = ("%.1f fps (%d frames) " % (td["fps"], td["frames"])) + note
            lines.append("| %s | %s | %s | %s |" % (r["name"], r["status"], r["duration_s"], note))
        probs = [(r["name"], p) for r in s["results"] for p in r.get("problems", [])]
        if probs:
            lines += ["", "Suspicious console lines:", ""]
            lines += ["- `%s`: `%s`" % (n, p[:200]) for n, p in probs[:30]]
    for c in comparisons:
        lines += ["", "## Images against %s" % c["other"], ""]
        if c["perf"]["a_fps"] and c["perf"]["b_fps"]:
            lines += ["Timedemo (best run): candidate %.1f fps, %s %.1f fps." % (
                c["perf"]["a_fps"], c["other"], c["perf"]["b_fps"]), ""]
        lines += ["| scenario | shot | % pixels differ | mean diff | max | flagged | triptych |",
                  "|---|---|---|---|---|---|---|"]
        for r in c["rows"]:
            if r.get("error"):
                lines.append("| %s | %s | %s | | | yes | |" % (r["scenario"], r["shot"], r["error"]))
            else:
                lines.append("| %s | %s | %.3f | %.3f | %d | %s | %s |" % (
                    r["scenario"], r["shot"], r["pct_pixels_differ"], r["mean_abs_diff"],
                    r["max_diff"], "yes" if r["flagged"] else "", r["triptych"]))
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    write_json(os.path.join(out, "findings.json"), {"verdict": verdict, "findings": findings})
    log("verdict: %s (%s)" % (verdict, os.path.join(out, "report.md")))
    return verdict


# ----------------------------------------------------------------------------------
# subcommands
# ----------------------------------------------------------------------------------

def cmd_build(args):
    src = os.path.abspath(args.src)
    build = os.path.join(src, "build")
    os.makedirs(build, exist_ok=True)
    cfg = ["cmake", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
    openal = "/opt/homebrew/opt/openal-soft"
    if os.path.isdir(openal):
        cfg += ["-DOPENAL_INCLUDE_DIR=%s/include" % openal,
                "-DOPENAL_LIBRARY=%s/lib/libopenal.dylib" % openal]
    subprocess.check_call(cfg + [os.path.join(src, "neo")], cwd=build, stdout=subprocess.DEVNULL)
    r = subprocess.run(["ninja"], cwd=build, capture_output=True, text=True)
    if r.returncode != 0:
        errs = [l for l in r.stdout.splitlines() if "error" in l or "FAILED" in l]
        sys.exit("build failed:\n" + "\n".join(errs[:40]))
    print(find_binary(build))


def common_setup(args):
    global BACKGROUND, EXTRA_CVARS
    BACKGROUND = not args.foreground
    EXTRA_CVARS = {k: v for k, v in (args.set or [])}
    args.data = os.path.abspath(os.path.expanduser(args.data))
    args.out = os.path.abspath(args.out)
    if not os.path.isfile(os.path.join(args.data, "base", "pak000.pk4")):
        sys.exit("error: no game data (base/pak000.pk4) under %s" % args.data)
    os.makedirs(args.out, exist_ok=True)


def cmd_run(args):
    common_setup(args)
    d = run_build(args, args.label, find_binary(args.bin), args.renderer)
    verdict = write_report(args.out, d, [], [])
    sys.exit(1 if verdict == "FAIL" else 0)


def cmd_compare(args):
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    c = compare_runs(os.path.abspath(args.a), os.path.abspath(args.b), out, args.strict)
    verdict = write_report(out, os.path.abspath(args.a), [c], [os.path.abspath(args.b)])
    sys.exit(1 if verdict == "FAIL" else 0)


def cmd_all(args):
    common_setup(args)
    binary = find_binary(args.bin)
    cand = run_build(args, "candidate", binary, args.renderer)
    comparisons, extra = [], []
    if args.renderer == "metal" and not args.no_gl:
        gl = run_build(args, "gl", binary, "opengl")
        comparisons.append(compare_runs(cand, gl, args.out, strict=False))
        extra.append(gl)
    if args.baseline_bin:
        base = run_build(args, "baseline", find_binary(args.baseline_bin), args.baseline_renderer or args.renderer)
        comparisons.append(compare_runs(cand, base, args.out, strict=True))
        extra.append(base)
    verdict = write_report(args.out, cand, comparisons, extra)
    sys.exit(1 if verdict == "FAIL" else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("--src", default=".")
    b.set_defaults(func=cmd_build)

    def common(sp):
        sp.add_argument("--bin", required=True)
        sp.add_argument("--data", default=DEFAULT_DATA)
        sp.add_argument("--out", default="metal-test-out")
        sp.add_argument("--renderer", default="metal", choices=["metal", "opengl"])
        sp.add_argument("--suite", default="quick", choices=["quick", "full"])
        sp.add_argument("--only", help="regex: run only matching scenarios")
        sp.add_argument("--res", nargs=2, type=int, default=list(DEFAULT_RES))
        sp.add_argument("--timeout", type=int, default=SCENARIO_TIMEOUT)
        sp.add_argument("--validation", action="store_true")
        sp.add_argument("--no-timedemo", action="store_true")
        sp.add_argument("--foreground", action="store_true",
                        help="let the game window come to the front and take focus")
        sp.add_argument("--keep-home", action="store_true")
        sp.add_argument("--set", nargs=2, action="append", metavar=("NAME", "VALUE"))

    r = sub.add_parser("run")
    common(r)
    r.add_argument("--label", default="candidate")
    r.set_defaults(func=cmd_run)

    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--out", default="metal-test-out")
    c.add_argument("--strict", action="store_true")
    c.set_defaults(func=cmd_compare)

    a = sub.add_parser("all")
    common(a)
    a.add_argument("--baseline-bin")
    a.add_argument("--baseline-renderer", choices=["metal", "opengl"])
    a.add_argument("--no-gl", action="store_true")
    a.set_defaults(func=cmd_all)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
