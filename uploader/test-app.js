/* Host test of the page (app.js) against a stand-in Cycles that answers the file commands from
   memory, with a minimal DOM: connect, list, make a folder, convert and upload, check what arrived.
   node uploader/test-app.js */
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const E = require("./elektron.js");

let failures = 0;
function check(cond, what) {
    if (!cond) {
        failures++;
        console.log("FAIL: " + what);
    }
}

/* ---- a minimal DOM ---------------------------------------------------------------------- */
function element(id) {
    const el = {
        id, textContent: "", className: "", value: "", disabled: false, hidden: false, maxLength: 0,
        children: [], onclick: null, onchange: null, oninput: null, title: "", files: [],
        appendChild(c) { this.children.push(c); return c; },
        click() { if (this.onclick) return this.onclick(); },
        get innerHTML() { return this._html || ""; },
        set innerHTML(v) { this._html = v; this.children = []; }
    };
    return el;
}
const ids = ["status", "in", "out", "connect", "back", "mkdir", "refresh", "pick", "chooser", "upload", "path", "folder", "files"];
const elements = {};
ids.forEach(id => { elements[id] = element(id); });

/* ---- a stand-in Cycles ------------------------------------------------------------------ */
const drive = new Map([["/", { folder: true }], ["/factory", { folder: true, protect: true }],
                       ["/incoming", { folder: true }]]);
const open = new Map();
let nextId = 7, log = [];
function children(dir) {
    const pre = dir === "/" ? "/" : dir + "/";
    return [...drive.keys()].filter(p => p !== dir && p.startsWith(pre) && !p.slice(pre.length).includes("/"));
}
function answer(m) {
    const seq = (m[0] << 8) | m[1], type = m[4], body = m.slice(5);
    const out = (bytes) => [0, 99, (seq >> 8) & 255, seq & 255, type | 0x80].concat(bytes);
    const str = (b, p) => { let e = p; while (b[e] !== 0) e++; return E.decodeText(b.slice(p, e)); };
    log.push(type);
    switch (type) {
    case E.TYPE.PING:
        return out([1]);
    case E.TYPE.READ_DIR: {
        const dir = str(body, 0), bytes = [];
        if (!drive.has(dir) || !drive.get(dir).folder)
            return out([]);
        children(dir).forEach(p => {
            const it = drive.get(p);
            bytes.push(...E.u32(it.folder ? 0 : 0x1235), ...E.u32(it.folder ? 0 : it.data.length),
                       it.protect ? 1 : 0, it.folder ? 0x44 : 0x46, ...E.encodeText(p.slice(p.lastIndexOf("/") + 1)), 0);
        });
        return out(bytes);
    }
    case E.TYPE.CREATE_DIR: {
        const p = str(body, 0);
        if (drive.has(p))
            return out([0]);
        drive.set(p, { folder: true });
        return out([1]);
    }
    case E.TYPE.OPEN_WRITE: {
        const size = E.readU32(body, 0), p = str(body, 4), parent = E.parentPath(p);
        if (!drive.has(parent) || drive.get(parent).protect)
            return out([0, ...E.encodeText("Write protected"), 0]);
        open.set(nextId, { path: p, size, data: new Uint8Array(size) });
        return out([1, ...E.u32(nextId++)]);
    }
    case E.TYPE.WRITE: {
        const id = E.readU32(body, 0), len = E.readU32(body, 4), off = E.readU32(body, 8), f = open.get(id);
        if (!f || off + len > f.size || len > E.BLOCK)
            return out([0]);
        f.data.set(body.slice(12, 12 + len), off);
        return out([1]);
    }
    case E.TYPE.CLOSE_WRITE: {
        const id = E.readU32(body, 0), f = open.get(id);
        if (!f)
            return out([0]);
        open.delete(id);
        drive.set(f.path, { folder: false, data: f.data });
        return out([1]);
    }
    /* a file marked inUse stands for the one the Cycles sometimes refuses until it restarts */
    case E.TYPE.DELETE_FILE: {
        const p = str(body, 0), it = drive.get(p);
        if (!it || it.folder || drive.get(E.parentPath(p)).protect)
            return out([0]);
        if (it.inUse)
            return out([0, ...E.encodeText("File in use"), 0]);
        drive.delete(p);
        return out([1]);
    }
    case E.TYPE.DELETE_DIR: {
        const p = str(body, 0), it = drive.get(p);
        if (p === "/" || !it || !it.folder || it.protect || children(p).length)
            return out([0]);
        drive.delete(p);
        return out([1]);
    }
    }
    return out([0]);
}
const input = { id: "in1", name: "Elektron Model:Cycles", manufacturer: "Elektron", onmidimessage: null };
const output = {
    id: "out1", name: "Elektron Model:Cycles", manufacturer: "Elektron",
    send(bytes) {
        const m = E.unframe(Uint8Array.from(bytes));
        const reply = E.frame(answer(m));
        /* in two pieces, with a clock byte in between, as MIDI may deliver it */
        setTimeout(() => {
            input.onmidimessage({ data: Uint8Array.from([...reply.slice(0, 5), 0xf8]) });
            input.onmidimessage({ data: reply.slice(5) });
        }, 0);
    }
};
const access = { inputs: new Map([["in1", input]]), outputs: new Map([["out1", output]]), onstatechange: null };

/* ---- the browser around the page -------------------------------------------------------- */
const tone = new Float32Array(48000);
for (let i = 0; i < tone.length; i++)
    tone[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / 48000);
/* a wavetable of two frames of 256, a sine then a saw: what model27's tools/wavetable.py makes */
const table = Float32Array.from({ length: 512 }, (_, k) => (k < 256 ? 0.999 * Math.sin(2 * Math.PI * k / 256)
                                                                  : (k - 256) / 128 - 1));
const sawFirst = Float32Array.from([...table.slice(256), ...table.slice(0, 256)]);
class OfflineAudioContext {
    constructor(ch, len, rate) { this.rate = rate; }
    async decodeAudioData(buf) {
        const kind = new Uint8Array(buf)[0];
        if (kind === 0x57)                                         /* "W": the wavetable, mono */
            return { numberOfChannels: 1, sampleRate: this.rate, length: table.length, getChannelData: () => table };
        if (kind === 0x58)                                         /* "X": the same frames, the saw first */
            return { numberOfChannels: 1, sampleRate: this.rate, length: sawFirst.length, getChannelData: () => sawFirst };
        if (kind !== 0x52)
            throw new Error("not audio");
        return { numberOfChannels: 2, sampleRate: this.rate, length: tone.length,
                 getChannelData: (c) => (c === 0 ? tone : new Float32Array(tone.length)) };
    }
}
function file(name, firstByte) {
    return { name, size: 1000, type: "audio/wav", arrayBuffer: async () => Uint8Array.from([firstByte, 1, 2]).buffer };
}
const sandbox = {
    window: null,
    document: {
        getElementById: (id) => elements[id],
        createElement: () => element(""),
        addEventListener: () => {}
    },
    navigator: { requestMIDIAccess: async () => access },
    OfflineAudioContext, setTimeout, clearTimeout, console, Uint8Array, Int16Array, Float32Array, DataView,
    Array, Math, Promise, Error, String, Object, Map
};
sandbox.window = sandbox;
let folderName = "My Samples";
sandbox.window.prompt = () => folderName;
let confirmAnswer = true, asked = [];
sandbox.window.confirm = (text) => { asked.push(text); return confirmAnswer; };
vm.createContext(sandbox);
for (const f of ["elektron.js", "app.js"])
    vm.runInContext(fs.readFileSync(path.join(__dirname, f), "utf8"), sandbox, { filename: f });

const settle = () => new Promise(r => setTimeout(r, 20));
async function until(cond, what) {
    for (let i = 0; i < 500 && !cond(); i++)
        await settle();
    check(cond(), what);
}
const names = (list) => list.children.map(li => (li.children[0] ? li.children[0].textContent : li.textContent));
const row = (name) => elements.folder.children.find(li => li.children[0] && li.children[0].textContent === name);
const under = (dir) => [...drive.keys()].filter(p => p === dir || p.startsWith(dir + "/"));
/* the × of a row, clicked: the event must not reach the row, which would open a folder */
let stopped = false;
const del = (name) => row(name).children[2].onclick({ stopPropagation() { stopped = true; } });
const state_files = () => elements.files.children.filter(li => li.children.length === 3).length;

(async () => {
    /* connect: ports picked, ping, root listed */
    elements.connect.onclick();
    await until(() => elements.status.className === "ok", "connects");
    check(elements.in.value === "in1" && elements.out.value === "out1", "the Cycles' ports are picked");
    check(JSON.stringify(names(elements.folder)) === '["factory","incoming"]', "the root is listed: " + names(elements.folder));
    check(elements.folder.children[0].children[1].textContent === "protected", "factory shows as protected");
    check(elements.back.disabled, "no Back at the root");

    /* a new folder, entered */
    elements.mkdir.onclick();
    await until(() => elements.path.textContent === "/My Samples", "the new folder is made and entered");
    check(drive.has("/My Samples"), "the stand-in has the folder");
    check(!elements.back.disabled, "Back in a folder");
    check(elements.folder.children.length === 0 && /This folder is empty\./.test(elements.folder.innerHTML),
          "an empty folder says so: " + elements.folder.innerHTML);

    /* two files: one audio, one not */
    elements.chooser.files = [file("Kick 01.wav", 0x52), file("notes.txt", 0x00)];
    elements.chooser.onchange();
    check(elements.files.children.length === 2, "two files listed");
    elements.upload.onclick();
    await until(() => /of 2 uploaded/.test(elements.status.textContent), "the upload finishes");
    check(elements.status.textContent.indexOf("1 of 2 uploaded") === 0, "one of two: " + elements.status.textContent);
    const states = elements.files.children.map(li => li.children[1].textContent);
    check(/^done/.test(states[0]), "the audio file is done: " + states[0]);
    check(/^failed: the browser cannot read/.test(states[1]), "the other one fails: " + states[1]);

    /* what arrived */
    const got = drive.get("/My Samples/Kick 01");
    check(got && !got.folder, "the file is on the drive under its name");
    if (got) {
        const dv = new DataView(got.data.buffer);
        check(got.data.length === 64 + 96000 + 16, "48000 frames with header and end: " + got.data.length);
        check(dv.getUint32(4) === 96000 && dv.getUint32(8) === 48000 && got.data[1] === 0, "header: bytes, rate, mono");
        let worst = 0;
        for (let i = 0; i < 48000; i++)
            worst = Math.max(worst, Math.abs(dv.getInt16(64 + 2 * i) - Math.round(tone[i] / 2 * 32768)));
        check(worst <= 1, "the frames are the mix of both channels, big-endian (off by " + worst + ")");
    }
    check(log.filter(t => t === E.TYPE.WRITE).length === Math.ceil((64 + 96000 + 16) / E.BLOCK), "written in 8-KiB blocks");

    /* the failed one leaves the list by its ×; a second upload of the same name is refused before
       anything is written */
    elements.files.children[1].children[2].onclick();
    check(elements.files.children.length === 1, "the failed file is removed from the list");
    const writes = log.length;
    elements.chooser.files = [file("Kick 01.aif", 0x52)];
    elements.chooser.onchange();
    elements.upload.onclick();
    await until(() => /of 1 uploaded/.test(elements.status.textContent), "the second upload ends");
    check(/already in this folder/.test(elements.files.children[1].children[1].textContent), "a name already there is refused");
    check(!log.slice(writes).includes(E.TYPE.OPEN_WRITE), "and nothing is opened for it");

    /* writing into a protected folder: the Cycles' refusal shows */
    elements.back.onclick();
    await until(() => elements.path.textContent === "/", "back at the root");
    elements.folder.children[0].onclick();                         /* factory */
    await until(() => elements.path.textContent === "/factory", "into factory");
    elements.files.children[1].children[2].onclick();
    elements.chooser.files = [file("Snare.wav", 0x52)];
    elements.chooser.onchange();
    elements.upload.onclick();
    await until(() => /of 1 uploaded/.test(elements.status.textContent), "the protected upload ends");
    const last = elements.files.children[elements.files.children.length - 1].children[1].textContent;
    check(/Write protected/.test(last), "the refusal is shown: " + last);
    check(!/warning/.test(elements.status.textContent), "no warning outside /Wavetables");

    /* /Wavetables, which model27's CHORD Tables plays: a table whose first frame is no sine is told */
    elements.back.onclick();
    await until(() => elements.path.textContent === "/", "back at the root again");
    folderName = "Wavetables";
    elements.mkdir.onclick();
    await until(() => elements.path.textContent === "/Wavetables", "/Wavetables is made and entered");
    while (state_files() > 0)
        elements.files.children[0].children[2].onclick();
    elements.chooser.files = [file("01.wav", 0x57), file("02.wav", 0x58)];
    elements.chooser.onchange();
    elements.upload.onclick();
    await until(() => /of 2 uploaded/.test(elements.status.textContent), "the two tables are uploaded");
    const tableStates = elements.files.children.map(li => li.children[1].textContent);
    check(/^done/.test(tableStates[0]) && !/warning/.test(tableStates[0]), "01, a sine first: done, no warning: " + tableStates[0]);
    check(/^done.*warning: its first frame is not a sine/.test(tableStates[1]), "02, a saw first: done, with the warning: " + tableStates[1]);
    check(elements.files.children[1].children[1].className === "info warn", "the warning in the warning colour");
    check(/2 of 2 uploaded to \/Wavetables\. 1 with a warning/.test(elements.status.textContent) &&
          elements.status.className === "warn", "the status line counts it: " + elements.status.textContent);
    check(drive.has("/Wavetables/01") && drive.has("/Wavetables/02"), "both are on the drive: a warning, not a refusal");

    /* a file CHORD Tables cannot play (a second of tone: 48000 samples, not frames of 256) is not written */
    while (state_files() > 0)
        elements.files.children[0].children[2].onclick();
    const before = log.length;
    elements.chooser.files = [file("03.wav", 0x52), file("04.wav", 0x57)];
    elements.chooser.onchange();
    elements.upload.onclick();
    await until(() => /of 2 uploaded/.test(elements.status.textContent), "the refused upload ends");
    const refusedStates = elements.files.children.map(li => li.children[1].textContent);
    check(/^failed: CHORD Tables cannot play it: 48000 samples are not 1 to 32 frames of 256.*not uploaded$/.test(refusedStates[0]),
          "03 is refused with the reason: " + refusedStates[0]);
    check(elements.files.children[0].children[1].className === "info bad", "the refusal in the error colour");
    check(!drive.has("/Wavetables/03") && drive.has("/Wavetables/04"), "03 is not on the drive, 04 is");
    check(log.slice(before).filter(t => t === E.TYPE.OPEN_WRITE).length === 1, "only 04 was opened for writing");
    check(/1 of 2 uploaded to \/Wavetables\. 1 not uploaded: CHORD Tables cannot play it\./.test(elements.status.textContent) &&
          elements.status.className === "bad", "the status line says it: " + elements.status.textContent);

    /* deleting a file: nothing goes without a yes */
    const deletes = () => log.filter(t => t === E.TYPE.DELETE_FILE || t === E.TYPE.DELETE_DIR).length;
    check(row("01") && row("01").children[2].title === "Delete from the Cycles" && !row("01").children[2].disabled,
          "a file's row has its ×");
    confirmAnswer = false;
    await del("01");
    check(stopped, "the × keeps its click from the row");
    check(/^Delete “01” from the Cycles\? This cannot be undone\.$/.test(asked[asked.length - 1]),
          "it asks first: " + asked[asked.length - 1]);
    check(deletes() === 0 && drive.has("/Wavetables/01"), "a no deletes nothing");
    confirmAnswer = true;
    del("01");
    await until(() => elements.status.textContent === "Deleted /Wavetables/01.", "a yes deletes it: " + elements.status.textContent);
    check(elements.status.className === "ok", "said in the ok colour");
    check(!drive.has("/Wavetables/01") && drive.has("/Wavetables/02"), "only 01 is gone from the drive");
    check(!row("01") && row("02") && elements.path.textContent === "/Wavetables", "and from the list; the page stays in the folder");

    /* the root: no × on what the Cycles protects, nor on /incoming, which it makes itself */
    elements.back.onclick();
    await until(() => elements.path.textContent === "/", "back at the root for deleting");
    check(row("factory").children[2].disabled && row("factory").children[2].className === "del kept", "factory has no ×");
    check(row("incoming").children[2].disabled && row("incoming").children[2].className === "del kept", "incoming has no ×");
    check(!row("My Samples").children[2].disabled && row("My Samples").children[2].className === "del", "My Samples has its ×");

    /* a folder with a folder in it goes with everything in it, the contents first */
    drive.set("/My Samples/Hats", { folder: true });
    drive.set("/My Samples/Hats/Hat 1", { folder: false, data: new Uint8Array(80) });
    drive.set("/My Samples/Hats/Hat 2", { folder: false, data: new Uint8Array(80) });
    const deleteLog = log.length;
    del("My Samples");
    await until(() => elements.status.textContent === "Deleted /My Samples.", "the folder is deleted: " + elements.status.textContent);
    check(/^Delete the folder “My Samples” and everything in it from the Cycles\?/.test(asked[asked.length - 1]),
          "it asks about the folder and everything in it: " + asked[asked.length - 1]);
    check(under("/My Samples").length === 0, "nothing of it is left: " + under("/My Samples"));
    const order = log.slice(deleteLog).filter(t => t === E.TYPE.DELETE_FILE || t === E.TYPE.DELETE_DIR);
    /* Kick 01, then Hats with its two files: the stand-in lists in the order things were made */
    check(JSON.stringify(order) === JSON.stringify([E.TYPE.DELETE_FILE, E.TYPE.DELETE_FILE, E.TYPE.DELETE_FILE,
                                                    E.TYPE.DELETE_DIR, E.TYPE.DELETE_DIR]),
          "three files, two folders, each folder after its contents: " + order.map(t => "0x" + t.toString(16)));
    check(!row("My Samples") && elements.path.textContent === "/", "it leaves the list; the page stays at the root");
    check(drive.has("/Wavetables/02") && drive.has("/incoming"), "the other folders stay");

    /* the Cycles refuses one: it stops there, says why, and lists what is left */
    drive.set("/Kit", { folder: true });
    drive.set("/Kit/a", { folder: false, data: new Uint8Array(80) });
    drive.set("/Kit/b", { folder: false, data: new Uint8Array(80), inUse: true });
    drive.set("/Kit/c", { folder: false, data: new Uint8Array(80) });
    elements.refresh.onclick();
    await until(() => !!row("Kit"), "the new folder is listed");
    del("Kit");
    await until(() => elements.status.className === "bad", "the refusal ends it");
    check(elements.status.textContent === "the Cycles did not delete /Kit/b: File in use", "the refusal is shown: " + elements.status.textContent);
    check(!drive.has("/Kit/a") && drive.has("/Kit/b") && drive.has("/Kit/c") && drive.has("/Kit"), "a went, b and what follows stay");
    check(!!row("Kit") && elements.path.textContent === "/", "the folder is still listed");
    check(!elements.connect.disabled && !row("Kit").children[2].disabled, "the page takes clicks again");

    /* the tree: into a folder in a folder by their rows, and back up one at a time */
    drive.set("/A", { folder: true });
    drive.set("/A/B", { folder: true });
    elements.refresh.onclick();
    await until(() => !!row("A"), "A is listed");
    row("A").onclick();
    await until(() => elements.path.textContent === "/A", "into A");
    check(JSON.stringify(names(elements.folder)) === '["B"]', "A lists B and nothing else: " + names(elements.folder));
    row("B").onclick();
    await until(() => elements.path.textContent === "/A/B", "into A/B");
    check(/This folder is empty\./.test(elements.folder.innerHTML), "B is empty and says so");
    elements.back.onclick();
    await until(() => elements.path.textContent === "/A", "Back goes to A");
    check(!elements.back.disabled, "and Back is still there");
    elements.back.onclick();
    await until(() => elements.path.textContent === "/", "Back again goes to the root");
    check(elements.back.disabled, "where Back is off");
    check(!!row("A") && !!row("Kit") && !!row("factory"), "the root is listed again");

    if (failures) {
        console.log(failures + " check(s) failed");
        process.exit(1);
    }
    console.log("all page tests passed");
})();
