/* Cycles One-Shot uploader: the page. Web MIDI to the Model:Cycles, the folders on its +Drive, audio
   converted in the browser and written with the device's own file commands (elektron.js). */
(function () {
    "use strict";
    var E = window.Elektron;
    var POOL = 63 * 1024 * 1024;    /* SAMPLE's sample memory: a bigger file would never load */
    var state = {
        access: null, input: null, output: null, seq: 1, pending: null, rx: null,
        path: "/", items: [], files: [], busy: false, connected: false
    };

    function $(id) { return document.getElementById(id); }
    function hex(n) { return "0x" + n.toString(16).padStart(2, "0"); }
    function show(text, kind) {
        $("status").textContent = text;
        $("status").className = kind || "";
    }
    function kb(n) { return n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : Math.ceil(n / 1024) + " KB"; }

    /* ---- MIDI ---------------------------------------------------------------------------- */

    function likely(p) { return /cycles|elektron/i.test(p.name + " " + (p.manufacturer || "")); }

    function fillPorts() {
        [["in", state.access.inputs], ["out", state.access.outputs]].forEach(function (pair) {
            var sel = $(pair[0]), keep = sel.value, ports = Array.from(pair[1].values()), pick = keep;

            sel.innerHTML = "";
            ports.forEach(function (p) {
                var o = document.createElement("option");

                o.value = p.id;
                o.textContent = p.name;
                sel.appendChild(o);
            });
            if (!ports.some(function (p) { return p.id === pick; })) {
                var guess = ports.filter(likely)[0] || ports[0];

                pick = guess ? guess.id : "";
            }
            sel.value = pick;
        });
    }

    async function connect() {
        if (!navigator.requestMIDIAccess) {
            show("This browser has no Web MIDI. Use Chrome or Edge.", "bad");
            return;
        }
        try {
            state.access = await navigator.requestMIDIAccess({ sysex: true });
        } catch (e) {
            show("MIDI access was refused (" + e.message + "). Allow MIDI with SysEx for this page.", "bad");
            return;
        }
        state.access.onstatechange = function () { fillPorts(); render(); };
        fillPorts();
        await usePorts();
    }

    async function usePorts() {
        if (state.input)
            state.input.onmidimessage = null;
        state.input = state.access.inputs.get($("in").value) || null;
        state.output = state.access.outputs.get($("out").value) || null;
        state.connected = false;
        if (!state.input || !state.output) {
            show("Pick the Model:Cycles as MIDI input and output.", "bad");
            render();
            return;
        }
        state.input.onmidimessage = onMidi;
        try {
            await send(E.TYPE.PING, [], 3000);
            state.connected = true;
            show("Connected.", "ok");
            await listDir(state.path);
        } catch (e) {
            show("The Cycles does not answer: " + e.message, "bad");
        }
        render();
    }

    /* SysEx may come in pieces, with real-time bytes in between. */
    function onMidi(ev) {
        var d = ev.data, i;

        for (i = 0; i < d.length; i++) {
            var b = d[i];

            if (b === 0xf0)
                state.rx = [0xf0];
            else if (b === 0xf7) {
                if (state.rx) {
                    state.rx.push(0xf7);
                    reply(new Uint8Array(state.rx));
                    state.rx = null;
                }
            } else if (b < 0xf8 && state.rx)
                state.rx.push(b);
        }
    }

    function reply(sysex) {
        var m = E.unframe(sysex), w = state.pending;

        if (!m || !w || E.replySeq(m) !== w.seq)
            return;
        state.pending = null;
        clearTimeout(w.timer);
        if (m[4] !== (w.type | 0x80))
            w.reject(new Error("unexpected reply " + hex(m[4]) + " to " + hex(w.type)));
        else
            w.resolve(m);
    }

    /* One request at a time; resolves with the reply's message. */
    function send(type, body, timeout) {
        return new Promise(function (resolve, reject) {
            var seq, w;

            if (!state.output)
                return reject(new Error("no MIDI output"));
            if (state.pending)
                return reject(new Error("still waiting for the last reply"));
            seq = state.seq;
            state.seq = (state.seq + 1) & 0xffff;
            w = { seq: seq, type: type, resolve: resolve, reject: reject };
            w.timer = setTimeout(function () {
                if (state.pending === w)
                    state.pending = null;
                reject(new Error("no reply within " + timeout / 1000 + " s"));
            }, timeout);
            state.pending = w;
            try {
                state.output.send(E.frame(E.request(seq, type, body)));
            } catch (e) {
                clearTimeout(w.timer);
                state.pending = null;
                reject(e);
            }
        });
    }

    /* ---- folders ------------------------------------------------------------------------- */

    /* A folder's entries, without moving the page there. */
    async function readDir(path) {
        return E.parseDir(await send(E.TYPE.READ_DIR, E.pathBytes(path), 5000));
    }

    async function listDir(path) {
        var items = await readDir(path);

        state.path = path;
        state.items = items.sort(function (a, b) {
            return a.folder !== b.folder ? (a.folder ? -1 : 1) : a.name.localeCompare(b.name);
        });
        render();
        return state.items;
    }

    async function makeFolder() {
        var name = E.cleanName(window.prompt("Name of the new folder") || ""), m;

        if (!name)
            return;
        m = await send(E.TYPE.CREATE_DIR, E.pathBytes(E.joinPath(state.path, name)), 5000);
        if (!E.replyOk(m))
            throw new Error(E.replyText(m) || "the Cycles did not make the folder");
        await listDir(E.joinPath(state.path, name));
        show("Folder " + state.path + " made.", "ok");
    }

    /* What the page offers no delete for: what the Cycles marks protected, and /incoming, which it
       makes when it formats its drive (the files in it can go). */
    function kept(it) {
        return it.writeProtected || (state.path === "/" && it.folder && it.name === "incoming");
    }

    /* The Cycles deletes only an empty folder, so its contents go first, one request at a time. */
    async function remove(path, folder) {
        var items, m, i;

        if (folder) {
            items = await readDir(path);
            for (i = 0; i < items.length; i++)
                await remove(E.joinPath(path, items[i].name), items[i].folder);
        }
        show("Deleting " + path + "…");
        m = await send(folder ? E.TYPE.DELETE_DIR : E.TYPE.DELETE_FILE, E.pathBytes(path), 30000);
        if (!E.replyOk(m))
            throw new Error("the Cycles did not delete " + path + (E.replyText(m) ? ": " + E.replyText(m) : ""));
    }

    async function deleteItem(it) {
        var path = E.joinPath(state.path, it.name), here = state.path, items;

        if (!window.confirm(it.folder ? "Delete the folder “" + it.name + "” and everything in it from the Cycles?" +
                                        " This cannot be undone."
                                      : "Delete “" + it.name + "” from the Cycles? This cannot be undone."))
            return;
        try {
            await remove(path, it.folder);
        } catch (e) {
            await listDir(here).catch(function () {});   /* show what is left */
            throw e;
        }
        items = await listDir(here);
        if (items.some(function (x) { return x.name === it.name; }))
            throw new Error(it.name + " is still in the folder afterwards");
        show("Deleted " + path + ".", "ok");
    }

    /* ---- files --------------------------------------------------------------------------- */

    function addFiles(list) {
        Array.from(list).forEach(function (f) {
            state.files.push({ file: f, name: E.cleanName(f.name), state: "", done: false });
        });
        render();
    }

    /* Decoded by the browser at the Cycles' rate, its channels averaged: the 16-bit frames. */
    async function convert(file) {
        var ctx = new OfflineAudioContext(1, 1, E.RATE), buf, ch = [], c;

        buf = await ctx.decodeAudioData(await file.arrayBuffer());
        for (c = 0; c < buf.numberOfChannels; c++)
            ch.push(buf.getChannelData(c));
        return E.toInt16(E.mixDown(ch));
    }

    function unplayable(n) {
        var e = new Error("CHORD Tables cannot play it: " + n + " samples are not 1 to " + E.TABLE_MAX +
                          " frames of " + E.CYCLE + " (a 48 kHz WAV of k \u00d7 256 samples, as model27's" +
                          " tools/wavetable.py makes it); not uploaded");

        e.unplayable = true;
        return e;
    }

    function progress(entry, text) {
        entry.state = text;
        renderFiles();
    }

    async function upload(entry) {
        var path = E.joinPath(state.path, entry.name), items, frames, data, m, id, off, chunk, found, warn = "";

        if (!entry.name)
            throw new Error("give it a name");
        items = await listDir(state.path);
        if (items.some(function (x) { return x.name === entry.name; }))
            throw new Error("something named " + entry.name + " is already in this folder");
        progress(entry, "converting");
        try {
            frames = await convert(entry.file);
        } catch (e) {
            throw new Error("the browser cannot read this audio file");
        }
        data = E.sampleFile(frames, E.RATE);
        /* CHORD Tables plays this folder: a file it cannot play is not written; a table whose first frame is no
           sine is written with a warning, since SWEEP 0 then plays no sine */
        if (state.path === E.WAVETABLES) {
            if (!E.tableFrames(frames))
                throw unplayable(frames.length);
            if (!E.startsWithSine(frames))
                warn = "its first frame is not a sine, so SWEEP 0 plays no sine with CHORD Tables";
        }
        if (data.length - 80 > POOL)
            throw new Error("too long for SAMPLE's memory (" + kb(data.length) + ")");
        progress(entry, "opening");
        m = await send(E.TYPE.OPEN_WRITE, E.u32(data.length).concat(E.pathBytes(path)), 10000);
        if (!E.replyOk(m))
            throw new Error(E.replyText(m) || "the Cycles did not open the file");
        id = E.readU32(m, 6);
        for (off = 0; off < data.length; off += E.BLOCK) {
            chunk = data.subarray(off, Math.min(off + E.BLOCK, data.length));
            m = await send(E.TYPE.WRITE, E.u32(id).concat(E.u32(chunk.length), E.u32(off), Array.from(chunk)), 10000);
            if (!E.replyOk(m))
                throw new Error("writing stopped at byte " + off);
            progress(entry, Math.floor((off + chunk.length) * 100 / data.length) + " %");
        }
        progress(entry, "finishing");
        m = await send(E.TYPE.CLOSE_WRITE, E.u32(id).concat(E.u32(data.length)), 60000);
        if (!E.replyOk(m))
            throw new Error(E.replyText(m) || "the Cycles did not finish the file");
        items = await listDir(state.path);
        found = items.filter(function (x) { return !x.folder && x.name === entry.name; })[0];
        if (!found)
            throw new Error("the file is not in the folder afterwards");
        if (found.size !== data.length)
            throw new Error("the Cycles has " + found.size + " bytes, " + data.length + " were sent");
        entry.done = true;
        entry.warn = warn;
        progress(entry, "done, " + kb(data.length) + (warn ? "; warning: " + warn : ""));
    }

    async function uploadAll() {
        var todo = state.files.filter(function (f) { return !f.done; }), ok = 0, warned = 0, refused = 0, i;

        for (i = 0; i < todo.length; i++) {
            try {
                show("Uploading " + todo[i].name + " (" + (i + 1) + " of " + todo.length + ")…");
                await upload(todo[i]);
                ok++;
                if (todo[i].warn)
                    warned++;
            } catch (e) {
                if (e.unplayable)
                    refused++;
                progress(todo[i], "failed: " + e.message);
            }
        }
        show(ok + " of " + todo.length + " uploaded to " + state.path + "." +
             (refused ? " " + refused + " not uploaded: CHORD Tables cannot play it." : "") +
             (warned ? " " + warned + " with a warning: see the list." : ""),
             ok !== todo.length ? "bad" : warned ? "warn" : "ok");
    }

    /* ---- the page ------------------------------------------------------------------------ */

    /* Runs fn while nothing else runs; its error goes to the status line. */
    function guarded(fn) {
        return async function () {
            var args = arguments;

            if (state.busy)
                return;
            state.busy = true;
            render();
            try {
                await fn.apply(null, args);
            } catch (e) {
                show(e.message, "bad");
            } finally {
                state.busy = false;
                render();
            }
        };
    }

    function renderFolder() {
        var list = $("folder"), up;

        $("path").textContent = state.path;
        list.innerHTML = "";
        if (!state.connected) {
            list.innerHTML = '<li class="empty">Not connected.</li>';
            return;
        }
        if (state.path !== "/") {
            up = document.createElement("li");
            up.className = "folder";
            up.textContent = "..";
            up.onclick = guarded(function () { return listDir(E.parentPath(state.path)); });
            list.appendChild(up);
        }
        state.items.forEach(function (it) {
            var li = document.createElement("li"), name = document.createElement("span"), info = document.createElement("span"),
                rm = document.createElement("button");

            name.textContent = it.name;
            info.className = "info";
            info.textContent = it.folder ? (it.writeProtected ? "protected" : "") : kb(it.size);
            /* kept rows hold an invisible one, so the columns stay in line */
            rm.className = "del" + (kept(it) ? " kept" : "");
            rm.textContent = "×";
            rm.title = "Delete from the Cycles";
            rm.disabled = state.busy || kept(it);
            rm.onclick = function (ev) {
                if (ev)
                    ev.stopPropagation();       /* a folder's row would open it */
                return guarded(deleteItem)(it);
            };
            li.appendChild(name);
            li.appendChild(info);
            li.appendChild(rm);
            if (it.folder) {
                li.className = "folder";
                li.onclick = guarded(function () { return listDir(E.joinPath(state.path, it.name)); });
            }
            list.appendChild(li);
        });
        if (!state.items.length && state.path === "/")
            list.innerHTML = '<li class="empty">The drive is empty.</li>';
    }

    function renderFiles() {
        var list = $("files");

        list.innerHTML = "";
        state.files.forEach(function (f, i) {
            var li = document.createElement("li"), input = document.createElement("input"),
                st = document.createElement("span"), rm = document.createElement("button");

            input.value = f.name;
            input.maxLength = E.NAME_MAX;
            input.disabled = state.busy || f.done;
            /* a typed name keeps its dots: cleanName drops only the dummy extension */
            input.oninput = function () { f.name = E.cleanName(input.value + ".x"); };
            st.className = "info" + (/^failed/.test(f.state) ? " bad" : f.warn ? " warn" : f.done ? " ok" : "");
            st.textContent = f.state || kb(f.file.size) + " " + (f.file.type || "");
            rm.textContent = "×";
            rm.title = "Remove from the list";
            rm.disabled = state.busy;
            rm.onclick = function () { state.files.splice(i, 1); render(); };
            li.appendChild(input);
            li.appendChild(st);
            li.appendChild(rm);
            list.appendChild(li);
        });
        if (!state.files.length)
            list.innerHTML = '<li class="empty">Add audio files: WAV, AIFF, MP3, FLAC… (drop them here)</li>';
    }

    function render() {
        var ready = state.connected && !state.busy;

        $("connect").disabled = state.busy;
        $("in").disabled = $("out").disabled = state.busy || !state.access;
        $("mkdir").disabled = !ready;
        $("refresh").disabled = !ready;
        $("pick").disabled = state.busy;
        $("upload").disabled = !ready || !state.files.some(function (f) { return !f.done; });
        renderFolder();
        renderFiles();
    }

    $("connect").onclick = guarded(connect);
    $("in").onchange = $("out").onchange = guarded(usePorts);
    $("mkdir").onclick = guarded(makeFolder);
    $("refresh").onclick = guarded(function () { return listDir(state.path); });
    $("pick").onclick = function () { $("chooser").click(); };
    $("chooser").onchange = function () { addFiles($("chooser").files); $("chooser").value = ""; };
    $("upload").onclick = guarded(uploadAll);
    document.addEventListener("dragover", function (e) { e.preventDefault(); });
    document.addEventListener("drop", function (e) {
        e.preventDefault();
        if (!state.busy && e.dataTransfer && e.dataTransfer.files.length)
            addFiles(e.dataTransfer.files);
    });
    render();
})();
