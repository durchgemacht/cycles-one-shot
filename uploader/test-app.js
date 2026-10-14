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
const ids = ["status", "in", "out", "connect", "mkdir", "refresh", "pick", "chooser", "upload", "path", "folder", "files"];
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
class OfflineAudioContext {
    constructor(ch, len, rate) { this.rate = rate; }
    async decodeAudioData(buf) {
        if (new Uint8Array(buf)[0] !== 0x52)
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
sandbox.window.prompt = () => "My Samples";
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

(async () => {
    /* connect: ports picked, ping, root listed */
    elements.connect.onclick();
    await until(() => elements.status.className === "ok", "connects");
    check(elements.in.value === "in1" && elements.out.value === "out1", "the Cycles' ports are picked");
    check(JSON.stringify(names(elements.folder)) === '["factory","incoming"]', "the root is listed: " + names(elements.folder));
    check(elements.folder.children[0].children[1].textContent === "protected", "factory shows as protected");

    /* a new folder, entered */
    elements.mkdir.onclick();
    await until(() => elements.path.textContent === "/My Samples", "the new folder is made and entered");
    check(drive.has("/My Samples"), "the stand-in has the folder");

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
    elements.folder.children[0].onclick();                         /* .. */
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

    if (failures) {
        console.log(failures + " check(s) failed");
        process.exit(1);
    }
    console.log("all page tests passed");
})();
