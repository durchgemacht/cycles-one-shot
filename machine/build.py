#!/usr/bin/env python3
"""SAMPLE 0.5: build a Model:Cycles OS 1.13 image with the sampler machine as its 7th machine.

The machine plays samples from the +Drive once per trig, through the one-shot core (src/smp_core.c)
and the bridge (machine/sampler.c). A sample is chosen for a track in the PRESET browser and saved
with the sound; machine/pool.c loads what the sounds name into free memory (0x42800000..0x46700000).
The machine is added to the OS the way Modded-Cycles adds its MACRO machine (tools/gen_macro.py,
MIT): the tables of machines, descriptors, names and update/render move into a payload at
0x46700000, which a boot hook in a freed sprite mask copies there at start-up; moveq bounds go up
by one machine, and detours teach the MACHINES page, the encoder and the sound records the new
machine. Five more writes lead into SAMPLE's own hooks (machine/hooks.S, pool.c): the browser's
file open, the sound record save (2 versions) and load, and the screen's redraw. Two vtable entries
lead the draw methods of the main screen and of the knob view through SAMPLE's view (machine/view.c),
which shows a SAMPLE track's waveform with its start and end. This script
imports the machinery from your clone of Modded-Cycles (it changes nothing there), compiles the
machine with the m68k-elf toolchain and writes

    build/sampler-tweak.json                  the tweak, in Modded-Cycles' format
    build/model-cycles_OS1.13-sampler-v0.5.syx

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

VERSION = "0.5"
HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = ROOT / "build"
STOCK_SHA256 = "44fe586269631a0ca7da25a3383fc6733c314809505fc3cc52f1e0ed9800640c"   # model-cycles_OS1.13.syx
MC_TESTED = "6a9742a"       # the Modded-Cycles commit this was built against (tools/gen_macro.py etc.)
BASE = 0x40000400

# The machine: knobs COLOR, SHAPE, SWEEP, CONTOUR = (long name, short name, default[, min, max]);
# Start and Length run to 120 as on the Model:Samples (SMP_SPAN in machine/pool.h);
# decay = default of its Amp Decay; image = the picture it borrows (the OS has only 6; 4 = TONE).
MACHINE = dict(name="SAMPLE", image=4, decay=127,
               knobs=(("Start", "STRT", 0, 0, 120), ("Length", "LEN", 120, 0, 120), ("Cutoff", "FREQ", 64),
                      ("Reso", "RESO", 0)))
FIRST_DESC = 76            # the descriptor of SAMPLE's first knob (COLOR = Start), after the OS's 76
SOURCES = (ROOT / "src" / "smp_core.c", ROOT / "src" / "smp_coeffs.c", ROOT / "src" / "smp_filter.c",
           ROOT / "src" / "smp_filter_tab.c", ROOT / "src" / "smp_filter_cf.S", HERE / "sampler.c", HERE / "pool.c",
           HERE / "view.c", HERE / "hooks.S")
CFLAGS = ["-std=c99", "-mcpu=54418", "-O2", "-ffreestanding", "-fno-builtin", "-fno-tree-loop-distribute-patterns",
          "-nostdlib", "-fno-pic", "-fno-common", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
          "-Werror", "-DSAMPLE_MACHINE=6", "-DDESC_START=%d" % FIRST_DESC]
BSS_OFF = 0x36000           # variables after the moved tables, as MACRO
PAY_SIZE = 0x100000         # the payload's place, 0x46700000..0x46800000; the samples' pool ends below it
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
    import build as mc_build    # noqa: E402
    import aplib_grow           # noqa: E402
    return gen_macro, mc_build, aplib_grow


def compile_machine(gm, tmp, pay):
    """Core, table, bridge, sample memory and hooks, linked at pay (code and data) and pay + BSS_OFF
    (variables). Returns (bytes from pay to the end of the data, symbols, end of the variables)."""
    gx, gs = gm.gx, gm.gs
    objs = []
    cutoff_object = "-DCUTOFF_OBJECT=%d" % (gm.g7.SNARE_DESC + 2)   # SAMPLE's knobs share SNARE's objects
    for src in SOURCES:
        o = tmp / (src.stem + ".o")
        gx.run([gx.CROSS + "gcc", *CFLAGS, cutoff_object, "-I%s" % (ROOT / "src"), "-I%s" % HERE, "-c", str(src),
                "-o", str(o)])
        objs.append(o)
    ld, elf, out = tmp / "link.ld", tmp / "sampler.elf", tmp / "sampler.bin"
    ld.write_text(LINK.format(code=pay, bss=pay + BSS_OFF))
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
    if pay + len(blob) > gs.STUBS:
        sys.exit("code and tables too big: %d bytes" % len(blob))
    bss = secs.get(".bss", (pay + BSS_OFF, 0))
    if bss[0] != pay + BSS_OFF:
        sys.exit("variables not at pay + 0x%x" % BSS_OFF)
    if bss[0] + bss[1] > pay + PAY_SIZE:
        sys.exit("variables end past 0x%x" % (pay + PAY_SIZE))
    return blob, syms, (bss[0] + bss[1] + 3) & ~3


def build_tweak(gm, img):
    """The tweak: gen_macro.build_tweak without Model-TG, with this machine in place of MACRO, at Model-TG's
    payload address (the memory below it holds the samples), and with SAMPLE's hooks."""
    gx, gs, g7, g8, sprites = gm.gx, gm.gs, gm.g7, gm.g8, gm.sprites
    pay = gs.PAY_TG
    if pay + PAY_SIZE != 0x46800000:
        sys.exit("Modded-Cycles moved its payload for Model-TG: 0x%x" % pay)
    gs.set_base(pay)
    nm, top, n = 7, 6, 1                        # machines: the 6 stock ones, then SAMPLE
    cur = img
    u32 = lambda va: struct.unpack_from(">I", cur, va - BASE)[0]
    m = MACHINE
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        blob, syms, bss_end = compile_machine(gm, tmp, pay)
        names_at, upd_at, rnd_at, map_at = gs.DATA, gs.DATA + 4 * nm, gs.DATA + 8 * nm, gs.DATA + 16 * nm
        stubs, ssyms = gm.assemble(tmp, "det", gs.detours_asm(n, [m["image"]], [FIRST_DESC], None), gs.STUBS)
        if gs.STUBS + len(stubs) > gs.DATA:
            sys.exit("detours too big")

        # data: names, update/render tables, machine -> record, strings
        strings = [m["name"]] + [k[i] for k in m["knobs"] for i in (0, 1)]
        at, addr = map_at + ((nm + 3) & ~3), {}
        for s_ in strings:
            if s_ not in addr:
                addr[s_] = at
                at += len(s_) + 1
        names = [u32(g7.NAMES + 4 * i) for i in range(6)] + [addr[m["name"]]]
        upd = [u32(g7.UPDATE_TAB + 4 * i) for i in range(6)] + [syms["sampler_update"]]
        rnd = [u32(g7.RENDER_TAB + 4 * i) for i in range(6)] + [syms["sampler_render"]]
        data = bytearray()
        for t in (names, upd, rnd, range(1, nm + 1)):
            data += b"".join(g7.be32(x) for x in t)
        data += cur[gs.MAP - BASE:gs.MAP - BASE + 6] + bytes(range(6, nm)) + bytes(((nm + 3) & ~3) - nm)
        for s_ in addr:
            data += s_.encode("ascii") + b"\0"
        if gs.DATA + len(data) > gs.DESCN:
            sys.exit("data too big")

        # 5 descriptors, modelled on SNARE's: COLOR, SHAPE, SWEEP, CONTOUR, Amp Decay
        new = bytearray()
        for k, (long_, short, default, *rng) in enumerate(m["knobs"]):
            e = bytearray(img[g7.DESC + (g7.SNARE_DESC + k) * g7.DSTRIDE - BASE:][:g7.DSTRIDE])
            e[0x00:0x04] = g7.be32(6)
            if rng:
                e[0x08:0x0c], e[0x0c:0x10] = g7.be32(rng[0] << 8), g7.be32(rng[1] << 8)
            e[0x10:0x14] = g7.be32(default << 8)
            e[0x2c:0x30] = g7.be32(addr[long_])
            e[0x34:0x38] = g7.be32(addr[short])
            new += e
        e = bytearray(img[g7.DESC + (g7.SNARE_DESC + 4) * g7.DSTRIDE - BASE:][:g7.DSTRIDE])
        e[0x10:0x14] = g7.be32(m["decay"] << 8)
        new += e

        # writes into the OS, each checked against the stock bytes
        writes = []

        def w(va, old, new_):
            if cur[va - BASE:va - BASE + len(old)] != old:
                sys.exit("0x%x: expected %s, found %s" % (va, old.hex(), cur[va - BASE:va - BASE + len(old)].hex()))
            writes.append({"off": va - BASE, "old": old.hex(), "new": new_.hex()})

        def moveq(va, old, new_, reg_byte):
            w(va, bytes([reg_byte, old]), bytes([reg_byte, new_]))

        def jmp(va):
            return bytes.fromhex("4ef9") + g7.be32(va)

        writes.append(None)                     # the boot hook, written below (it carries the list of chunks)
        red = sprites.redirect_write(gx.CAVE)
        w(BASE + red["off"], bytes.fromhex(red["old"]), bytes.fromhex(red["new"]))
        w(gx.HOOK, bytes.fromhex(gx.HOOK_OLD), jmp(gx.CAVE) + bytes.fromhex("4e71"))
        moved = {g7.DESC: gs.DESCN, g7.DESC + 8: gs.DESCN + 8, g7.DESC + 0x20: gs.DESCN + 0x20, g7.ROWS: gs.ROWSN,
                 g7.CCROWS: gs.CCROWSN, g7.NAMES: names_at, g7.UPDATE_TAB: upd_at, g7.RENDER_TAB: rnd_at,
                 gs.MAP: map_at}
        want = {g7.DESC: 34, g7.DESC + 8: 1, g7.DESC + 0x20: 2, g7.ROWS: 5, g7.CCROWS: 2, gs.MAP: 1,
                g7.RENDER_TAB: 1}
        for old, new_ in moved.items():
            rs = g7.refs32(cur, old)
            if len(rs) != want.get(old, 1):
                sys.exit("references to 0x%x: %d" % (old, len(rs)))
            for va in rs:
                w(va, g7.be32(old), g7.be32(new_))
        jumps = g7.JUMPS + g8.JUMPS8
        for va in g7.BOUNDS:
            if va in {j[0] for j in jumps}:
                continue
            b0, b1 = cur[va - BASE], cur[va - BASE + 1]
            if b0 & 0xf1 != 0x70 or b1 not in (75, 76):
                sys.exit("bound 0x%x" % va)
            moveq(va, b1, b1 + 5 * n, b0)
        for va, reg in ((0x400a7dba, 0x72), (0x400a7df4, 0x70), (0x4005a6a6, 0x72), (0x400147a4, 0x70),
                        (0x400148aa, 0x72), (0x400148b2, 0x70), (0x400a25e0, 0x70)):
            if cur[va - BASE] != reg or cur[va - BASE + 1] not in (5, 6):
                sys.exit("bound 0x%x" % va)
            moveq(va, cur[va - BASE + 1], top, reg)
        moveq(0x4005a572, 5, 6, 0x72)
        w(0x4005a2b8, bytes.fromhex("487800c0"), bytes.fromhex("4878") + (32 * nm).to_bytes(2, "big"))
        moveq(0x400a26e8, cur[0x400a26e8 - BASE + 1], nm, 0x70)
        for va in (0x4001b69c, 0x400a40a6, 0x400a4fb0):
            moveq(va, 5, 1, 0x70)
        for va, old, sym in jumps:
            w(va, bytes.fromhex(old), jmp(ssyms[sym]))
        for va, old, sym in g7.CALLS:
            w(va, bytes.fromhex(old), bytes.fromhex("4eb9") + g7.be32(ssyms[sym]) + bytes.fromhex("4e71"))
        for va, old, op, sym in HOOKS:
            new_ = (bytes.fromhex("4eb9") + g7.be32(syms[sym]) if op == "jsr"
                    else jmp(syms[sym]) + bytes.fromhex("4e71"))
            if len(new_) != len(old) // 2:
                sys.exit("hook at 0x%x: %d bytes for %d" % (va, len(new_), len(old) // 2))
            w(va, bytes.fromhex(old), new_)
        for va, method, sym in VIEWS:
            if g7.refs32(cur, method) != [va]:
                sys.exit("references to 0x%x: %s" % (method, [hex(r) for r in g7.refs32(cur, method)]))
            w(va, g7.be32(method), g7.be32(syms[sym]))

        # the payload: our code and data, the detours, the data block, the stock descriptors (copied from
        # your file at build time) and the new ones
        parts = [
            {"dest": "%#x" % pay, "hex": blob.hex()},
            {"dest": "%#x" % gs.STUBS, "hex": stubs.hex()},
            {"dest": "%#x" % gs.DATA, "hex": bytes(data).hex()},
            {"dest": "%#x" % gs.DESCN, "cycles": ["%#x" % g7.DESC, "%#x" % (g7.DESC + g7.NDESC * g7.DSTRIDE)]},
            {"dest": "%#x" % (gs.DESCN + g7.NDESC * g7.DSTRIDE), "hex": bytes(new).hex()},
        ]
        lo_, hi_ = g7.DESC - BASE, g7.DESC - BASE + g7.NDESC * g7.DSTRIDE
        final, orig = bytearray(cur[lo_:hi_]), img[lo_:hi_]
        alg = g7.ALG_DESC * g7.DSTRIDE + 0x0c
        if struct.unpack_from(">I", final, alg)[0] != 5 << 8:
            sys.exit("maximum of the Algorithm parameter")
        struct.pack_into(">I", final, alg, top << 8)
        reloc = [["%#x" % (gs.DESCN + k), orig[k:k + 4].hex(), final[k:k + 4].hex()]
                 for k in range(0, len(final), 4) if final[k:k + 4] != orig[k:k + 4]]
        size = bss_end - pay
        segs = gs.segments(parts, size)
        at = BASE + gx.IMAGE_LEN
        if at + sum(n_ for _, n_ in segs) > gs.END_LIMIT:
            sys.exit("adding %d bytes would end the image past 0x%x" % (sum(n_ for _, n_ in segs), gs.END_LIMIT))
        (tmp / "segs.inc").write_text("".join("\t.long\t%#x, %d\n" % (a, n_ // 4) for a, n_ in segs))
        stub, _ = gm.assemble(tmp, "stub", gx.SRC / "stub.S", gx.CAVE, [
            "-DPAYLOAD_SRC=%#x" % at, "-DPAYLOAD_DST=%#x" % pay, "-DPAYLOAD_LONGS=%d" % (size // 4), "-DPACK",
            "-I%s" % tmp])
    if len(stub) > sprites.zone(gx.CAVE)[1]:
        sys.exit("boot hook too big for its place")
    if cur[gx.CAVE - BASE:gx.CAVE - BASE + len(stub)] != b"\xff" * len(stub):
        sys.exit("the boot hook's place is taken")
    writes[0] = {"off": gx.CAVE - BASE, "old": "ff" * len(stub), "new": stub.hex()}
    writes.sort(key=lambda x: x["off"])
    for a_, b_ in zip(writes, writes[1:]):
        if a_["off"] + len(a_["new"]) // 2 > b_["off"]:
            sys.exit("writes overlap at 0x%x" % (BASE + b_["off"]))

    # Model-TG and Modded-Cycles' track filter hook the same sound record functions; sample preview needs
    # Model-TG and plays sample files from the same browser
    ids = gm.other_ids() | {"macro", "macro-tg", "model-tg", "model-tg-st", "track-filter", "sample-preview",
                            "sample-preview-st"}
    tweak = {
        "id": "sampler",
        "order": 26,
        "name": "SAMPLE machine %s: one-shot sampler for samples on the +Drive" % VERSION,
        "description": [
            "7th machine after the 6 stock ones. Plays a sample from the +Drive once per trig through the",
            "one-shot core of Cycles One-Shot (6-tap polyphase at 96 kHz, then the stock decimator). A sample",
            "is chosen for a SAMPLE track in the PRESET browser and saved with the sound; stereo files play",
            "as mono, other rates at their pitch. COLOR = start, SHAPE = length, 0..120 each as on the",
            "Model:Samples (it ends at start + length); DECAY, GATE and PUNCH: the stock amp chain set up",
            "like TONE. On a SAMPLE track the main screen shows the sample's name and waveform, start and",
            "end marked; turning COLOR or SHAPE moves the marks. Samples live in 0x42800000..0x46700000,",
            "the payload above.",
            "Machine mechanics from Modded-Cycles' MACRO (tools/gen_macro.py).",
        ],
        "device": "Model:Cycles",
        "os": "1.13",
        "section": 3,
        "conflicts": sorted(ids),
        "writes": writes,
        "append": {"at": "%#x" % at, "dest": "%#x" % pay, "size": size, "parts": parts, "reloc": reloc,
                   "pack": [["%#x" % a_, n_] for a_, n_ in segs]},
        "symbols": {k: "%#x" % syms[k] for k in ("sampler_update", "sampler_render") + tuple(h[3] for h in HOOKS)
                    + tuple(v[2] for v in VIEWS)},
    }
    info = dict(code=len(blob), bss=bss_end - pay - BSS_OFF, image=sum(n_ for _, n_ in segs))
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
    gm, mcb, aplib_grow = load_mc(args.modded_cycles)
    img = gm.g7.cycles_main(args.stock)
    if len(img) != gm.gx.IMAGE_LEN:
        sys.exit("not the MAIN OS 1.13")

    tweak, info = build_tweak(gm, img)
    BUILD.mkdir(exist_ok=True)
    (BUILD / "sampler-tweak.json").write_text(json.dumps(tweak, indent=1) + "\n", encoding="utf-8")
    print("tweak: %d writes; code and tables %d bytes, variables %d bytes, %d bytes added to the image"
          % (len(tweak["writes"]), info["code"], info["bss"], info["image"]))
    print("  sampler_update %s, sampler_render %s" % (tweak["symbols"]["sampler_update"],
                                                       tweak["symbols"]["sampler_render"]))

    out, patched = build_syx(mcb, aplib_grow, args.modded_cycles.resolve(), raw, tweak)
    path = BUILD / ("model-cycles_OS1.13-sampler-v%s.syx" % VERSION)
    path.write_bytes(out)
    report = {"version": VERSION, "stock": {"file": args.stock.name, "sha256": STOCK_SHA256},
              "main_os": {"sha256": sha(patched), "bytes": len(patched), "end": "%#x" % (BASE + len(patched))},
              "output": {"file": path.name, "sha256": sha(out), "bytes": len(out)}, "tweak": info}
    path.with_suffix(".syx.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print("wrote %s (%d bytes, SHA-256 %s)" % (path, len(out), sha(out)))
    print("  MAIN OS %d bytes, ends at %#x, SHA-256 %s" % (len(patched), BASE + len(patched), sha(patched)))
    if args.tool:
        check_with_tool(args.tool, path, patched)


if __name__ == "__main__":
    main()
