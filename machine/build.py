#!/usr/bin/env python3
"""SAMPLE 0.8: build a Model:Cycles OS 1.13 image with the sampler machine as its 7th machine.

The machine plays samples from the +Drive once per trig, through the one-shot core (src/smp_core.c)
and the bridge (machine/sampler.c). A sample is chosen for a track in the PRESET browser and saved
with the sound; machine/pool.c loads what the sounds name into free memory (the pool, POOL below).

Since 0.7 the machine is registered through Modded-Cycles' own registrar for added machines,
gen_macro.added_machine (MIT), the one MACRO and ACID use: it moves the tables of machines,
descriptors, names and update/render into a payload at 0x43000000 (gs.PAY_ALONE), which a boot
hook in a freed sprite mask copies there at start-up, raises the moveq bounds and assembles the
detours that teach the MACHINES page, the encoder and the sound records the new machine. SAMPLE's
code is linked at that payload's start, its variables above the moved tables, and the sample pool
starts 8 MB above the payload window so that other machines (MACRO, ACID) can share the window in
a combined build. Five more writes lead into SAMPLE's own hooks (machine/hooks.S, pool.c): the
browser's file open, the sound record save (2 versions) and load, and the screen's redraw. Two
vtable entries lead the draw methods of the main screen and of the knob view through SAMPLE's view
(machine/view.c), which shows a SAMPLE track's waveform with its start and end.

What this tweak cannot be installed with is computed, not listed by hand: every tweak of the clone
that writes other bytes to the same addresses, or whose payload runs in SAMPLE's payload window or
in its sample pool, is a conflict. This script imports the machinery from your clone of
Modded-Cycles (it changes nothing there), compiles the machine with the m68k-elf toolchain and
writes

    build/sampler-tweak.json                  the tweak, in Modded-Cycles' format
    build/model-cycles_OS1.13-sampler-v0.11.syx

It starts from your own copy of the official OS file (checked by its SHA-256). Modded-Cycles'
build steps check every write against the stock bytes, refuse writes into free space the OS
still points at, and re-sign the result; this script then reads the result back. With --tool,
elektron-firmware-tool checks checksums and HMAC independently and unpacks the MAIN OS again.

    python3 machine/build.py --stock model-cycles_OS1.13.syx --modded-cycles PATH/Modded-Cycles \\
        [--tool PATH/elektron-firmware-tool]
"""
import argparse
import hashlib
import hmac
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

VERSION = "0.11"
HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = ROOT / "build"
STOCK_SHA256 = "44fe586269631a0ca7da25a3383fc6733c314809505fc3cc52f1e0ed9800640c"   # model-cycles_OS1.13.syx
MC_TESTED = "95b63b7"       # the Modded-Cycles commit this was built against (gen_macro.added_machine etc.)
BASE = 0x40000400

# The machine: knobs COLOR, SHAPE, SWEEP, CONTOUR = (long name, short name, default[, min, max]);
# Start and Length run to 120 as on the Model:Samples (SMP_SPAN in machine/pool.h);
# decay = default of its Amp Decay; image = the picture it borrows (the OS has only 6; 4 = TONE).
MACHINE = dict(name="SAMPLE", image=4, decay=127,
               knobs=(("Start", "STRT", 0, 0, 120), ("Length", "LEN", 120, 0, 120), ("Cutoff", "FREQ", 64),
                      ("Reso", "RESO", 0)))
# The sample pool: RAM the Cycles OS leaves unused, from 8 MB above the added machines' payload window
# (gs.PAY_ALONE = 0x43000000) to where Model-TG's payload would start. A build whose payload ends above
# POOL[0] must move the pool (pool.c reads POOL_BASE / POOL_LIMIT).
POOL = (0x43800000, 0x46700000)
SOURCES = (ROOT / "src" / "smp_core.c", ROOT / "src" / "smp_coeffs.c", ROOT / "src" / "smp_filter.c",
           ROOT / "src" / "smp_filter_tab.c", ROOT / "src" / "smp_filter_cf.S", HERE / "sampler.c", HERE / "pool.c",
           HERE / "view.c", HERE / "hooks.S")
CFLAGS = ["-std=c99", "-mcpu=54418", "-O2", "-ffreestanding", "-fno-builtin", "-fno-tree-loop-distribute-patterns",
          "-nostdlib", "-fno-pic", "-fno-common", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
          "-Werror"]
# SAMPLE's hooks: (address, stock bytes, "jsr" or "jmp", symbol). A jmp replaces the first 8 bytes of a
# function (lea -12(sp),sp ; movem.l a2-a4,(sp)), which the hook runs itself (hooks.S).
HOOKS = ((0x400a6602, "4eb9400a3052", "jsr", "smp_hook_open"),       # the browser opens a file
         (0x4005afa0, "4feffff448d71c00", "jmp", "smp_hook_save2"),  # sound -> record, version 2
         (0x4005b054, "4feffff448d71c00", "jmp", "smp_hook_save1"),  # sound -> record, version 1
         (0x4005aecc, "4feffff448d71c00", "jmp", "smp_hook_load"),   # record -> sound
         (0x400075f0, "4eb940076be4", "jsr", "smp_hook_tick"))       # the screen's redraw, about 30 Hz
# SAMPLE's view: (vtable entry, stock method, symbol). Entry +16 is a view's draw method, called by the
# screen's redraw (0x40076c76) with the view and the screen's Bitmap; each entry is the only reference
# to its method, and SAMPLE's calls the stock one for anything else.
VIEWS = ((0x400ffe30, 0x4001b22a, "smp_main_draw"),       # MainScreenView (vtable 0x400ffe20)
         (0x4010053c, 0x4001e40a, "smp_knob_draw"))       # the knob view (vtable 0x4010052c)
# Tweaks that share the PRESET browser's file open with SAMPLE (they need Model-TG, which the byte check
# already refuses); named here so that the reason stays visible.
EXTRA_CONFLICTS = {"sample-preview", "sample-preview-st"}
ORDER = 27                  # after MACRO (25) and ACID (26): a chain of added machines keeps its payloads apart
LINK = """SECTIONS
{{
  .text {code:#x} : {{ *(.text.sampler_update) *(.text.sampler_render) *(.text*) }}
  .rodata : {{ *(.rodata*) }}
  .data : {{ *(.data*) }}
  .bss {bss:#x} (NOLOAD) : {{ *(.bss*) *(COMMON) }}
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
"""


def sha(b):
    return hashlib.sha256(b).hexdigest()


def load_mc(path):
    """Modded-Cycles' generator and build modules, from the clone at path."""
    tools = pathlib.Path(path).resolve() / "tools"
    if not (tools / "gen_macro.py").is_file():
        sys.exit("%s: not a clone of Modded-Cycles (no tools/gen_macro.py)" % path)
    head = subprocess.run(["git", "-C", str(tools.parent), "rev-parse", "--short=7", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    if head and head != MC_TESTED:
        print("note: Modded-Cycles is at %s, this was built against %s" % (head, MC_TESTED))
    sys.path[:0] = [str(tools), str(tools / "emu")]
    import gen_macro            # noqa: E402  (also brings gx, gs, g7, g8, sprites)
    if not hasattr(gen_macro, "added_machine"):
        sys.exit("%s: this Modded-Cycles has no gen_macro.added_machine (older than release 1.53); use %s or later"
                 % (path, MC_TESTED))
    import build as mc_build    # noqa: E402
    import aplib_grow           # noqa: E402
    return gen_macro, mc_build, aplib_grow, head


def compile_machine(gm, tmp, code_at, bss_at, code_end, machine_index, desc_start):
    """Core, table, bridge, sample memory and hooks, linked at code_at (code and data) and bss_at
    (variables); the code must end before code_end. machine_index is SAMPLE's index among the
    machines and desc_start its first descriptor, both set by the registrar's layout. Returns
    (bytes from code_at to the end of the data, symbols, end of the variables)."""
    gx, gs = gm.gx, gm.gs
    objs = []
    defs = ["-DSAMPLE_MACHINE=%d" % machine_index, "-DDESC_START=%d" % desc_start,
            "-DCUTOFF_OBJECT=%d" % (gm.g7.SNARE_DESC + 2),   # added machines' knobs share SNARE's objects (notes/18)
            "-DPOOL_BASE=%#xu" % POOL[0], "-DPOOL_LIMIT=%#xu" % POOL[1]]
    for src in SOURCES:
        o = tmp / (src.stem + ".o")
        gx.run([gx.CROSS + "gcc", *CFLAGS, *defs, "-I%s" % (ROOT / "src"), "-I%s" % HERE, "-c", str(src),
                "-o", str(o)])
        objs.append(o)
    ld, elf, out = tmp / "link.ld", tmp / "sampler.elf", tmp / "sampler.bin"
    ld.write_text(LINK.format(code=code_at, bss=bss_at))
    keep = [a for sym in [h[3] for h in HOOKS] + [v[2] for v in VIEWS] for a in ("-u", sym)]
    gx.run([gx.CROSS + "ld", "-T", str(ld), "--gc-sections", "--no-warn-rwx-segments", "-e", "sampler_update",
            "-u", "sampler_render", *keep, "-o", str(elf), *map(str, objs)])
    secs = {}
    for line in gx.run([gx.CROSS + "objdump", "-h", str(elf)]).splitlines():
        f = line.split()
        if len(f) > 4 and f[0].isdigit():
            secs[f[1]] = (int(f[3], 16), int(f[2], 16))      # address, size
    extra = set(secs) - {".text", ".rodata", ".data", ".bss"}
    if extra:
        sys.exit("unexpected sections: %s" % sorted(extra))
    gx.run([gx.CROSS + "objcopy", "-O", "binary", "-R", ".bss", str(elf), str(out)])
    blob = out.read_bytes()
    syms = {}
    for line in gx.run([gx.CROSS + "nm", str(elf)]).splitlines():
        p_ = line.split()
        if len(p_) == 3:
            syms[p_[2]] = int(p_[0], 16)
    if code_at + len(blob) > code_end:
        sys.exit("code and tables too big: %d bytes" % len(blob))
    bss = secs.get(".bss", (bss_at, 0))
    if bss[0] != bss_at:
        sys.exit("variables not at %#x" % bss_at)
    return blob, syms, (bss[0] + bss[1] + 3) & ~3


def own_writes(gm, img, syms):
    """SAMPLE's hooks and view entries as writes, each checked against the stock bytes."""
    g7 = gm.g7
    writes = []
    for va, old, op, sym in HOOKS:
        old = bytes.fromhex(old)
        new = (bytes.fromhex("4eb9") + g7.be32(syms[sym]) if op == "jsr"
               else bytes.fromhex("4ef9") + g7.be32(syms[sym]) + bytes.fromhex("4e71"))
        if len(new) != len(old):
            sys.exit("hook at 0x%x: %d bytes for %d" % (va, len(new), len(old)))
        if img[va - BASE:va - BASE + len(old)] != old:
            sys.exit("0x%x: expected %s, found %s" % (va, old.hex(), img[va - BASE:va - BASE + len(old)].hex()))
        writes.append({"off": va - BASE, "old": old.hex(), "new": new.hex()})
    for va, method, sym in VIEWS:
        if g7.refs32(img, method) != [va]:
            sys.exit("references to 0x%x: %s" % (method, [hex(r) for r in g7.refs32(img, method)]))
        writes.append({"off": va - BASE, "old": g7.be32(method).hex(), "new": g7.be32(syms[sym]).hex()})
    return writes


def computed_conflicts(mc_root, writes, ram):
    """The ids of the clone's tweaks this one cannot be installed with: a write of other bytes to the
    same address, or a payload that runs in one of our RAM ranges (the payload window, the pool).
    The same whole write (a sprite mask freed by both) is not a conflict, as in check_overlaps.py."""
    mine = {}
    for w in writes:
        new = bytes.fromhex(w["new"])
        for i, b in enumerate(new):
            mine[w["off"] + i] = b
    ids = set()
    for f in sorted((mc_root / "tweaks" / "model-cycles_OS1.13").glob("*.json")):
        if f.name == "device.json":
            continue
        t = json.loads(f.read_text(encoding="utf-8"))
        tid = t.get("id")
        if not tid:
            continue
        clash = False
        for w in t.get("writes", []):
            new = bytes.fromhex(w["new"])
            off = w["off"]
            for i, b in enumerate(new):
                if mine.get(off + i, b) != b:
                    clash = True
                    break
            if clash:
                break
        ap = t.get("append")
        if ap and not clash:
            lo, hi = int(ap["dest"], 16), int(ap["dest"], 16) + ap["size"]
            clash = any(lo < b and a < hi for a, b in ram)
        if clash:
            ids.add(tid)
    return ids


def build_tweak(gm, img, mc_root, mc_head):
    """The tweak: SAMPLE registered through gen_macro.added_machine as the 7th machine, at the alone
    payload window, with SAMPLE's own hooks and view entries, and the conflicts computed from the
    clone's tweaks."""
    gs, g7 = gm.gs, gm.g7
    pay = gs.PAY_ALONE
    gs.set_base(pay)
    first, n = 6, 1                             # machines: the 6 stock ones, then SAMPLE
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        blob, syms, bss_end = compile_machine(gm, tmp, pay, pay + gm.BSS_OFF, gs.STUBS, first, 76)
        if bss_end > POOL[0]:
            sys.exit("the payload ends at %#x, inside the sample pool (%#x): move POOL" % (bss_end, POOL[0]))
        machine = dict(MACHINE, update=syms["sampler_update"], render=syms["sampler_render"])
        writes, append = gm.added_machine(img, None, [machine], tmp, [(pay, blob)], bss_end)
    writes += own_writes(gm, img, syms)
    writes.sort(key=lambda x: x["off"])
    for a_, b_ in zip(writes, writes[1:]):
        if a_["off"] + len(a_["new"]) // 2 > b_["off"]:
            sys.exit("writes overlap at 0x%x" % (BASE + b_["off"]))
    ram = [(pay, bss_end), POOL]
    conflicts = computed_conflicts(mc_root, writes, ram) | EXTRA_CONFLICTS
    conflicts.discard("sampler")
    tweak = {
        "id": "sampler",
        "order": ORDER,
        "name": "SAMPLE machine %s: one-shot sampler for samples on the +Drive" % VERSION,
        "description": [
            "7th machine after the 6 stock ones, registered through gen_macro.added_machine like MACRO and ACID.",
            "Plays a sample from the +Drive once per trig through the one-shot core of Cycles One-Shot (6-tap",
            "polyphase at 96 kHz, then the stock decimator), the amp chain set up like TONE, then the Model:Samples'",
            "filter. A sample is chosen for a SAMPLE track in the PRESET browser and saved with the sound; stereo",
            "files play as mono, other rates at their pitch. COLOR = start, SHAPE = length, 0..120 each as on the",
            "Model:Samples (it ends at start + length); SWEEP = cutoff, CONTOUR = resonance; DECAY, GATE and PUNCH:",
            "the stock amp chain. On a SAMPLE track the main screen shows the sample's name and waveform, start and",
            "end marked. Samples live in %#x..%#x (%d MB); the payload runs at %#x." % (POOL[0], POOL[1],
                                                                                        (POOL[1] - POOL[0]) >> 20, pay),
            "Conflicts computed from the clone's tweaks (Modded-Cycles %s): other bytes at the same address, or a"
            % (mc_head or MC_TESTED),
            "payload in SAMPLE's payload window or sample pool. Generated by machine/build.py of Cycles One-Shot.",
        ],
        "device": "Model:Cycles",
        "os": "1.13",
        "section": 3,
        "conflicts": sorted(conflicts),
        "writes": writes,
        "append": append,
        "symbols": {k: "%#x" % syms[k] for k in ("sampler_update", "sampler_render") + tuple(h[3] for h in HOOKS)
                    + tuple(v[2] for v in VIEWS)},
    }
    info = dict(code=len(blob), bss=bss_end - pay - gm.BSS_OFF, image=sum(n_ for _, n_ in append["pack"]),
                pay=pay, bss_end=bss_end)
    return tweak, info


def build_syx(mcb, aplib_grow, mc_root, raw, tweak):
    """Applies the tweak to the stock file with Modded-Cycles' build steps; returns (syx, MAIN OS)."""
    stream, info = mcb.unwrap(raw)
    c = mcb.container.parse(stream)
    s3 = next(s for s in c["sections"] if s["id"] == 3)
    main_os, ops = mcb.aplib.depack(c["blob"][s3["off"]:s3["off"] + s3["size"]])
    dev = json.loads((mc_root / "tweaks" / "model-cycles_OS1.13" / "device.json").read_text(encoding="utf-8"))
    if sha(main_os) != dev["section_sha256"]:
        sys.exit("the MAIN OS is not the official 1.13")
    patched, dirty = mcb.apply_writes(main_os, [tweak])
    mcb.check_caves(main_os, [tweak], False, dirty, patched, dev.get("cave_refs_ok"))
    payload, _ = mcb.build_payload([tweak], main_os, None)
    orig_len = len(patched)
    patched += payload
    dirty += bytearray(b"\x01") * len(payload)
    new_s3 = aplib_grow.repack_grow(patched, ops, dirty, orig_len)
    if mcb.aplib.depack(new_s3)[0] != patched:
        sys.exit("repacked MAIN OS reads back different")
    msg = c["blob"][:len(c["blob"]) - mcb.container.DIGEST_LEN]
    expect = c["blob"][len(c["blob"]) - mcb.container.DIGEST_LEN:]
    plain = []
    for s in c["sections"]:
        try:
            plain.append(mcb.aplib.depack(c["blob"][s["off"]:s["off"] + s["size"]])[0])
        except Exception:
            pass
    key = mcb.container.find_key(plain, msg, expect)
    if not key:
        sys.exit("HMAC key not found")
    blob = mcb.container.rebuild(c, {3: new_s3}, key)
    out = mcb.wrap(mcb.container.build_stream(blob, mcb.BYTES_PER_MSG), info["product"], info["start_seq"])

    # read it back: packet checksums, container, MAIN OS, HMAC
    stream2, _ = mcb.unwrap(out)
    c2 = mcb.container.parse(stream2)
    s3b = next(s for s in c2["sections"] if s["id"] == 3)
    if mcb.aplib.depack(c2["blob"][s3b["off"]:s3b["off"] + s3b["size"]])[0] != patched:
        sys.exit("the written file does not unpack to the patched MAIN OS")
    body, mac = c2["blob"][:len(c2["blob"]) - mcb.container.DIGEST_LEN], c2["blob"][len(c2["blob"]) - mcb.container.DIGEST_LEN:]
    if hmac.new(key, body, hashlib.sha256).digest() != mac:
        sys.exit("HMAC of the written file does not verify")
    return out, patched


def check_with_tool(tool, path, patched):
    res = subprocess.run([tool, "-i", str(path)], capture_output=True, text=True)
    print("  elektron-firmware-tool: " + " | ".join(l.strip() for l in res.stdout.splitlines() if l.strip())[-200:])
    if res.returncode != 0:
        sys.exit("elektron-firmware-tool rejects the file")
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([tool, "-i", str(path), "-d", "3", "-o", d], check=True, capture_output=True)
        if (pathlib.Path(d) / "section_3_MAIN_OS.bin").read_bytes() != patched:
            sys.exit("elektron-firmware-tool unpacks a different MAIN OS")
    print("  elektron-firmware-tool unpacks the same MAIN OS")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--stock", type=pathlib.Path, required=True, help="your official model-cycles_OS1.13.syx")
    ap.add_argument("--modded-cycles", type=pathlib.Path, required=True, help="your clone of Modded-Cycles")
    ap.add_argument("--tool", help="elektron-firmware-tool, for an independent check")
    args = ap.parse_args()

    raw = args.stock.read_bytes()
    if sha(raw) != STOCK_SHA256:
        sys.exit("%s is not the official Model:Cycles OS 1.13 file (SHA-256 %s...)" % (args.stock, STOCK_SHA256[:16]))
    gm, mcb, aplib_grow, mc_head = load_mc(args.modded_cycles)
    img = gm.g7.cycles_main(args.stock)
    if len(img) != gm.gx.IMAGE_LEN:
        sys.exit("not the MAIN OS 1.13")

    mc_root = args.modded_cycles.resolve()
    tweak, info = build_tweak(gm, img, mc_root, mc_head)
    BUILD.mkdir(exist_ok=True)
    (BUILD / "sampler-tweak.json").write_text(json.dumps(tweak, indent=1) + "\n", encoding="utf-8")
    print("tweak: %d writes; code and tables %d bytes, variables %d bytes, %d bytes added to the image; payload "
          "%#x..%#x, pool %#x..%#x; %d conflicts computed"
          % (len(tweak["writes"]), info["code"], info["bss"], info["image"], info["pay"], info["bss_end"],
             POOL[0], POOL[1], len(tweak["conflicts"])))
    print("  sampler_update %s, sampler_render %s" % (tweak["symbols"]["sampler_update"],
                                                       tweak["symbols"]["sampler_render"]))

    out, patched = build_syx(mcb, aplib_grow, mc_root, raw, tweak)
    path = BUILD / ("model-cycles_OS1.13-sampler-v%s.syx" % VERSION)
    path.write_bytes(out)
    report = {"version": VERSION, "stock": {"file": args.stock.name, "sha256": STOCK_SHA256},
              "modded_cycles": mc_head or MC_TESTED,
              "main_os": {"sha256": sha(patched), "bytes": len(patched), "end": "%#x" % (BASE + len(patched))},
              "output": {"file": path.name, "sha256": sha(out), "bytes": len(out)}, "tweak": info}
    path.with_suffix(".syx.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print("wrote %s (%d bytes, SHA-256 %s)" % (path, len(out), sha(out)))
    print("  MAIN OS %d bytes, ends at %#x, SHA-256 %s" % (len(patched), BASE + len(patched), sha(patched)))
    if args.tool:
        check_with_tool(args.tool, path, patched)


if __name__ == "__main__":
    main()
