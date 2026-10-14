# Cycles Sampler

A sampler machine for the Elektron Model:Cycles, OS 1.13. Its sound core plays 16-bit mono samples
once from a start to an end frame (one shot), at any pitch from −72 to +36 semitones. It sits on the
Cycles as a 7th machine, SAMPLE. Version 0.2 plays samples from the +Drive: you choose one per track
in the PRESET browser, and it is saved with the sound.

**Status (2026-10-14):**

- Sound core and table: done and tested on the host. Every frame is checked bit for bit against an
  independent reference, DC passes exactly, and playback stops cleanly at the end.
- Machine 0.1 (built-in test samples): tested in the digiemu emulator. It boots, SAMPLE plays, and
  knobs, machine changes and several tracks work.
- Machine 0.2 (samples from the +Drive): builds into an OS image that passes every check of the
  build. Its bridge and sample memory pass their host tests. Tested in the digiemu emulator
  (2026-10-14): samples open from the PRESET browser and play, PITCH works, the project keeps them
  across saving and loading, two tracks play different samples, presets load on the other machines
  as before, and files at 44.1 and 46.875 kHz play at their pitch (heard as a steady tone against a
  48 kHz copy). Stereo files were not tried (digiemu's tool writes mono). Tested on a Model:Cycles
  (2026-10-14) with samples left on its +Drive by the Model:Samples OS: it boots, SAMPLE is on the
  MACHINES page, samples open from the browser and play, PITCH, Start and Length work, several
  tracks play different samples, and a saved project plays its samples again after switching the
  device off and on.
- Not SAMPLE's doing: when loading a project asks "Save … before loading?", Yes saves the project
  you leave but does not load the other one; loading it again works. The official OS 1.13 does
  the same, in digiemu and on a Model:Cycles.

## The machine (0.2)

SAMPLE is the 7th machine on the MACHINES page, with TONE's picture. A trig plays the track's sample
once:

| Knob | SAMPLE |
|---|---|
| PITCH, FINE, the trig's note | pitch, as on every machine. Note 60 at PITCH 64 and FINE 64 plays the sample as recorded; a sample recorded at another rate keeps its pitch |
| COLOR (Start) | where it starts, 0 = the beginning |
| SHAPE (Length) | how much of the rest it plays, 127 = to the end |
| SWEEP, CONTOUR | nothing yet |
| DECAY, GATE, PUNCH | the stock amp chain, set up as on TONE; DECAY starts at 127 so a sample plays out |

**Choosing a sample.** Set a track to SAMPLE, open the PRESET browser and open a sample file on the
+Drive. The track's sound keeps everything else, its name included, and now names that file. Sample
files are the ones the Model:Samples and Elektron Transfer write: a 64-byte header, then 16-bit
big-endian PCM. A stereo file plays as mono (left and right mixed). On any other machine, and for
presets, the browser works as before.

**Where the choice is kept.** In the sound itself, in three parameter words the OS leaves unused (k
25..27): the file's inode and its id. The id is the hash of the file's content that the file system
keeps in the inode, so it names the file even if it moves. The sound records in projects, kits and
presets keep the three words in record slots 24..26, which the OS writes as zero and never reads.
So the choice survives saving, loading and copying a sound, also into the sound pool for sound
locks.

**Loading.** From the screen's redraw, about 30 times a second, the machine looks at every sound
that could play: the kits of the 96 patterns, the kit the audio side is playing and the tracks' live
sounds. It loads every sample a SAMPLE sound names into free memory, 63 MB from `0x42800000`, and
frees what no sound names any more once no voice plays it. It reads 128 KB a tick through the file
system's own read, so a fragmented file reads correctly. If a file is no longer at its inode, the
file system's own index of ids says where it is now. A trig whose sample is not (yet) loaded stays
silent. The tick leaves the drive alone while the file system is busy in the same task or not
mounted.

**How it is built in.** Modded-Cycles adds its MACRO machine with a generator (`tools/gen_macro.py`,
MIT). `machine/build.py` uses the same machinery, imported from your clone of Modded-Cycles, which
it leaves unchanged:

- The tables of machines, parameter descriptors, names and `update`/`render` move into a payload at
  `0x46700000`, the place Modded-Cycles uses next to Model-TG. The samples get the 63 MB below it.
- A boot hook in a freed sprite mask copies the payload there at start-up.
- The `moveq` bounds go up by one machine.
- Detours teach the MACHINES page, the encoder and the sound records the new machine.

Five more writes lead into SAMPLE's own code:

| Where | What it was | What SAMPLE adds |
|---|---|---|
| `0x400a6602` | the browser reads the opened file as a preset | on a SAMPLE track, a sample file instead names the sample in the track's sound (`machine/hooks.S`, `smp_browser_open`) |
| `0x4005afa0`, `0x4005b054` | a sound becomes a record (versions 2 and 1) | the three words go into the record |
| `0x4005aecc` | a record becomes a sound | the three words come back |
| `0x400075f0` | the screen's redraw asks whether to draw | the loading tick runs first (`machine/pool.c`) |

The audio side is `machine/sampler.c`. At a trig it reads the sample's id from the parameters the
audio side has copied for the note, finds the loaded sample and starts it. It renders 64 frames at
96 kHz through the core, then calls the stock decimator and the amp chain like TONE. Version 0.2
leaves out two optional writes that MACRO carries: `voice_loop` (a shorter halving loop with the
same values) and `usb_steady` (an even USB audio clock). It cannot be combined with Model-TG or with
Modded-Cycles' track filter, which hook the same sound record functions.

**Build:**

```sh
python3 machine/build.py --stock model-cycles_OS1.13.syx --modded-cycles path/to/Modded-Cycles \
    --tool path/to/elektron-firmware-tool
```

This writes `build/model-cycles_OS1.13-sampler-v0.2.syx`, plus a `.json` report and the tweak in
Modded-Cycles' format (`build/sampler-tweak.json`). Every write is checked against the stock bytes,
and writes into free space that the OS still points at are refused. The result is read back
(checksums, HMAC, MAIN OS); with `--tool`, elektron-firmware-tool checks it again. Built against
Modded-Cycles commit `6a9742a`. `make machine-test` runs the host tests of the bridge and the sample
memory, with stand-ins for the OS.

**Trying it in digiemu.** digiemu's LOAD SAMPLES button is on the Model:Samples only. For the
Cycles, put WAV files on the emulated +Drive with digiemu's own tool, while digiemu is closed, after
the first start of this firmware has formatted its card:

```sh
python3 tools/ekfsadd.py path/to/firmware-folder/plusdrive.img kick.wav snare.wav
```

They land in `/incoming`, converted to mono; the rate is kept. The launcher then offers a rebuild.

**What to try first:** a sample on a SAMPLE track from the browser; trigs, pitch, Start and Length;
a 44.1 kHz file at note 60 (same pitch as recorded); saving and loading the project; another track
or pattern with another sample; a pool sound with a sample as a sound lock; presets on other
machines, as before.

## How it plays

The Model:Samples OS 1.13 plays a sample in two stages, and this core uses the same two:

1. **Interpolation at twice the rate.** For each block of 32 frames at 48 kHz the core computes 64
   frames at 96 kHz. Each frame comes from 6 neighbouring sample frames, weighted by one of 256 rows
   of a table; the top 8 bits of the playhead's fraction pick the row (`src/smp_core.c`).
2. **2:1 decimation.** The Cycles' own decimator in the stock voice chain (`0x400a9f58` in OS 1.13)
   brings the 64 frames down to 32. It is an IIR half-band filter: the even frames are delayed, the
   odd ones go through a 4th-order allpass, and the two are added. It is flat to 20 kHz (−0.01 dB)
   and attenuates by about 52 dB from 30 kHz up. The Model:Samples uses the same code with the
   same coefficients.

The playhead is 32.32 fixed point. Before each block the core copies the frames it will read into a
scratch buffer and puts silence wherever they fall outside the start–end window, so the inner loop
needs no bounds checks. The voice switches off once every tap has passed the end. A full-scale sample frame comes out as
`32767 << 15`, half of Q31, the level Cycles machines hand to the decimator.

**Cost:** with gcc 16 at `-O2` the inner loop takes 31 instructions per frame, about 2,200 per
track and block. The decimator adds about 520. For comparison, a TONE voice takes 5,291
(measured by Modded-Cycles). An EMAC loop like the one in the Model:Samples (16 instructions per
frame) would halve the core's share. That is planned for the integration step.

## The table

The table is one least-squares prototype filter of 1,536 taps (`tools/design.c`), cut into 256 rows
of 6 taps. Each row is centred on the playhead and sums to exactly 32768. The Makefile holds the
design in use:

```
DESIGN := proto 0.34 0.55 100 0   # passband to 16.3 kHz, stopband from 26.4 kHz, weight 100
```

### How clean it plays

`tests/measure.c` plays a sine at −1 dBFS at each pitch and tone. It reports the SINAD in dB: the
tone against everything else up to about 21 kHz. Higher is cleaner. The columns compare four ways
of playing:

- **Model-TG:** takes the frame under the playhead and holds it for two frames.
- **Linear:** interpolates linearly between two frames.
- **Model:Samples 1.13:** its own table, read from your copy of the OS (see below).
- **This core.**

| pitch, tone out | Model-TG | linear | Model:Samples 1.13 | this core |
|---|---:|---:|---:|---:|
| −24 st, 1 kHz | 18.3 | 39.4 | 62.8 | 56.4 |
| −24 st, 5 kHz | 1.6 | 5.5 | 19.8 | 54.3 |
| −12 st, 5 kHz | 11.1 | 22.1 | 56.7 | 55.0 |
| −12 st, 10 kHz | 2.8 | 5.5 | 20.1 | 69.1 |
| −5 st, 1 kHz | 27.5 | 77.6 | 66.4 | 75.1 |
| −5 st, 10 kHz | 8.7 | 34.6 | 51.6 | 55.2 |
| +5 st, 1 kHz | 32.1 | 87.7 | 71.5 | 80.7 |
| +5 st, 10 kHz | 12.4 | 47.8 | 58.3 | 60.7 |
| +5 st, 15 kHz | 6.4 | 27.7 | 53.8 | 53.4 |

Octaves (0, ±12, +24 st) land on whole or half frames, so they play cleanly at low tones with any
method; `make measure` prints the full grid. Over that grid the core's worst cell is 53.4 dB and
its mean 84.1 dB. The Model:Samples table reaches 19.8 dB and 79.5 dB. Above 5 kHz both are near
the limit that 256 rows set.

### How it sounds

The Model:Samples table is not flat. It rises to +1.9 dB around 10 kHz. A least-squares design that
damps the images of the source spectrum strongly takes on almost the same bump by itself, so the
bump looks like a by-product of that trade, not an EQ choice. This core keeps the bump up to 8 kHz.
Above that it is somewhat darker, and in exchange it damps the images more:

| kHz | 2 | 5 | 8 | 10 | 12 | 15 | 17 | 20 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Model:Samples 1.13, dB | +0.19 | +0.98 | +1.72 | +1.87 | +1.60 | +0.24 | −1.38 | −5.04 |
| this core, dB | +0.20 | +0.96 | +1.58 | +1.56 | +1.02 | −0.93 | −3.15 | −8.15 |

| image of the source spectrum at kHz | 26 | 28 | 30 | 34 | 40 | 44 |
|---|---:|---:|---:|---:|---:|---:|
| Model:Samples 1.13, dB | −18.0 | −24.8 | −33.6 | −76.1 | −68.2 | −73.1 |
| this core, dB | −29.5 | −101.2 | −41.4 | −81.7 | −58.0 | −64.6 |

`make sweep` rates other designs. `proto 0.36 0.55 100 0` is brighter and closer to the Model:Samples
above 10 kHz, but less clean when the pitch goes an octave or more down.

## Build and test

You need a C compiler and, for the target check, the m68k-elf toolchain (Homebrew:
`m68k-elf-gcc`, `m68k-elf-binutils`).

```sh
make test          # unit tests of the core on the host
make target        # compile for the ColdFire: size, and undefined symbols (must be none)
make measure       # the SINAD and gain grid for Model-TG's way, linear and the core
make coeffs        # regenerate src/smp_coeffs.c from DESIGN in the Makefile
make check-coeffs  # check that src/smp_coeffs.c matches DESIGN
```

To compare with the Model:Samples, take its table from your own copy of the official
`model-samples_OS1.13.syx`. You also need
[elektron-firmware-tool](https://github.com/mischa85/elektron-firmware-tool):

```sh
python3 tools/samples_table.py --stock model-samples_OS1.13.syx --tool path/to/elektron-firmware-tool
make measure SAMPLES=build/samples_table.bin
make sweep SAMPLES=build/samples_table.bin
```

The script checks the SHA-256 of the file and of its MAIN OS, and the bytes of the code that
reads the table. It writes `build/samples_table.bin` and the four decimator coefficients in
`build/samples_decimator.bin`. Both stay out of the repository.

### Listening tests

`make render` builds `build/render`. It brings a WAV file to 48 kHz/16 bit, the way the device
would hold it. Then it plays the file at a pitch: through the core, through another table, or
the way Model-TG plays (`--zoh`). Last comes a model of the Cycles' decimator. All methods come out
equally loud.

```sh
build/render --decimator build/samples_decimator.bin --semitones -12 in.wav core.wav
build/render --decimator build/samples_decimator.bin --table build/samples_table.bin --semitones -12 in.wav samples.wav
build/render --decimator build/samples_decimator.bin --zoh --semitones -12 in.wav model-tg.wav
```

## What the Model:Samples OS 1.13 does

These are notes from reading the MAIN OS, for the integration step:

- The audio block (`0x400586ae`) runs a fixed chain for 7 voices: one for preview and the 6 tracks.
  Each voice keeps 92 bytes of state from `0x8000cbe4`. Unlike the Cycles, the Samples has no
  machine framework.
- Sample data streams by eDMA from the 64 MB pool into SRAM buffers, about 140 frames per voice
  and block. The next voice loads while the current one computes.
- Interpolation at `0x400a7082`: 6 taps, 256 rows, `macw`, 16 instructions per frame. The table
  sits in SRAM at `0x8000a000`, in three banks of 1 KB that each hold two taps per row. The reset
  code copies it there from the image at `0x401A44F0`.
- Decimation at `0x400a714e` uses the same code and coefficients as the Cycles at `0x400a9f58`.
- Each track then gets an envelope (`0x400a5b28`) and a 2-pole filter: lowpass for knob values up
  to 16383, highpass above (`0x400a5f5a`, `0x400a5c1c`).
- The sample table holds 2,500 slots of 16 bytes from `0x404a2ad0`: data pointer, rate (48000 by
  default), length, and the rate ratio.

## License

MIT, see [LICENSE](LICENSE). The repository holds no Elektron firmware. The Model:Samples table is
read from your own copy of the OS file, used only for comparison, and stays in `build/`.
