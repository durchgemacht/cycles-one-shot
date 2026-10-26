# Cycles One-Shot

A one-shot sampler machine for the Elektron Model:Cycles, OS 1.13. It adds SAMPLE as a 7th machine
next to the six stock ones and plays samples from the +Drive. You choose a sample in the OS's own
PRESET browser, and it is saved with the sound.

The goal for now is the Model:Samples' sampler on the Cycles, in sound and in features. The sound
is there: SAMPLE plays a sample the way the Model:Samples does, with 6-tap interpolation at twice
the rate, then a 2:1 decimator. Nothing of it is copied from Elektron. The interpolation and its
table are our own design and code; the decimator is the one the Cycles OS already has, which SAMPLE
calls where it is (the Model:Samples uses the same). Start, Length and the filter work as on the
Model:Samples; the filter follows its measured laws with our own design and code. Still to come
from the Model:Samples: looping and reverse playback.

## Features

- **SAMPLE, a 7th machine.** It sits on the MACHINES page next to the stock machines and works
  with the sequencer, locks and the amp chain like them.
- **Samples from the +Drive.** Choose a sample in the PRESET browser on a SAMPLE track. The sound
  takes the sample's name and keeps the choice in projects, kits and presets. Hold a step while you
  open a sample, and the step plays it as a sample lock.
- **Clean playback.** 6-tap polyphase interpolation at 96 kHz, as on the Model:Samples, then the
  Cycles' own decimator. A sample recorded at another rate keeps its pitch, and stereo files play
  as mono.
- **Start and Length as on the Model:Samples.** Both run from 0 to 120, so moving Start keeps the
  length and 120 splits evenly into slices.
- **No clicks at the edges.** 4 ms fades at the end of what a trig plays, and at its start when
  Start lies inside the sample. A sample played from its first frame keeps its attack.
- **The Model:Samples' filter.** One knob, SWEEP, closes a lowpass to the left of 64 and a highpass
  to the right; CONTOUR adds resonance, up to a +27 dB peak at the cutoff. Frequency, resonance
  and how the resonance thins out towards the top follow the Model:Samples, measured on its OS
  and rebuilt from scratch: a 2-pole filter on the ColdFire's EMAC, about 800 instructions per
  track and block. At 0 (the default, the knob's middle) the track is untouched, bit for bit.
  Across the middle and from one side to the other the filter fades rather than switches, so
  a square, ramp or random LFO on Cutoff, a p-lock or the knob itself does not click.
  One difference: the filter runs inside the machine, after the amp envelope and before
  Volume + Dist, where the Model:Samples has it after. Volume + Dist is a stage of the OS's
  track chain that runs after every machine, so putting the filter behind it would mean changing
  that chain for all tracks. You hear the difference only with DIST above 0: the distortion
  then acts on the filtered signal. The laws, the structure and the measurements are in
  [TECHNICAL.md, section 16](TECHNICAL.md#16-the-filter).
- **A waveform view.** On a SAMPLE track the main screen shows a narrow column on the left: the
  current pattern's number (A01 to F16), TRAK with the track's number, and the track's level as a
  bar that moves while you turn LEVEL. To its right the sample's name sits in a box like the OS's
  own browser title, and below it the waveform. Lines mark where a trig starts and stops; they move
  while you turn Start and Length.
- **An upload page.** `uploader/` puts audio files on the +Drive over USB from Chrome or Edge, as
  samples SAMPLE plays, and deletes samples and folders from it (the × on a row, after asking; a
  folder goes with everything in it). It needs no firmware change, so it works with the official OS as well. In the
  folder `/Wavetables`, which model27's CHORD Tables plays as wavetables, it does not write a file
  CHORD Tables cannot play (not 1 to 32 frames of 256 samples, as a 48 kHz WAV), and it warns about a
  file whose first frame is not a sine: on such a table SWEEP 0 plays no sine.

| Knob | SAMPLE |
|---|---|
| PITCH (in tenths; FUNC + PITCH in semitones), the trig's note | pitch, as on every machine. PITCH 0 at note 60 plays the sample as recorded |
| COLOR | Start, 0–120 |
| SHAPE | Length, 0–120; it ends at Start + Length, at most at the sample's end. 120 (the default) plays to the end |
| SWEEP | Cutoff, shown as −64 … 63 like the Model:Samples: below 0 a lowpass that closes towards −64 (5 Hz), above 0 a highpass that closes towards 63 (19 kHz); 0 = off |
| CONTOUR | Reso, resonance 0–127 |
| DECAY, GATE, PUNCH | the stock amp chain, set up as on TONE. DECAY starts at 127, so a sample plays out; GATE is heard with a lower DECAY |

## Status

- **Machine 0.10** (a column on the left with the pattern's number, the track's number and its
  level; the waveform 105 pixels wide to its right): tested on a Model:Cycles (2026-10-25), and in
  the digiemu emulator.
- **Machine 0.9** (the bar above the waveform split in two boxes: the current pattern's number on
  the left, the sample's name on the right): tested on a Model:Cycles (2026-10-24), and in the
  digiemu emulator.
- **Machine 0.8** (for builders that compose machines from compiled objects, like model27's: with
  `CM_SYMBOLS` defined, the machine index and the first descriptor are undefined symbols the builder
  resolves when it places the object, and each assembly file has a section of its own; without it, the
  build is byte for byte the 0.7 build): One-Shot's own build is unchanged, the symbol build tested in
  the digiemu emulator through model27's composer (2026-10-21), alone and next to a second added
  machine; not yet on a device.
- **Machine 0.7** (registered through Modded-Cycles' own registrar for added machines, the one MACRO and
  ACID use, so that a combined build can hold several added machines; the payload moves to `0x43000000`
  and the sample pool to `0x43800000`–`0x46700000`, 47 MB; what it conflicts with is computed from the
  clone's tweaks instead of listed by hand): host-tested, checked with Modded-Cycles' own overlap
  checker, registry and build, alone and with fifteen other mods, and tested on a Model:Cycles
  (2026-10-21).
- **Machine 0.6** (the filter fades across the middle and between its sides instead of
  switching, after a report that a square, ramp or random LFO on Cutoff clicked in 0.5): tested
  on a Model:Cycles (2026-10-20), and in the digiemu emulator.
- **Machine 0.5** (the filter, Cutoff shown as −64 … 63): tested on a Model:Cycles (2026-10-18),
  and in the digiemu emulator. A sound saved by 0.4 or earlier loads with the filter off.
- **Machine 0.4** (waveform view, Start and Length as on the Model:Samples, fades): tested on a
  Model:Cycles (2026-10-17), and in the digiemu emulator.
- **Upload page 0.2** (deleting files and folders): host tests against a stand-in Cycles, and
  tested on a Model:Cycles (2026-10-26). A sample that is loaded plays on until no sound names it or the Cycles
  restarts; after that a sound that names it stays silent.
- **Upload page 0.1:** tested with a Model:Cycles (Chrome on macOS).
- **Limits:** SAMPLE is a tweak of its own, so it cannot be installed next to another tweak that adds
  machines (MACRO, ACID, the Syntakt engines, the SD Vintage variants) until a combined build exists;
  nor with Model-TG or Modded-Cycles' track filter, which hook the same sound record functions of the
  OS; nor with sample preview. The build computes that list from the clone's tweaks. Every other mod of
  Modded-Cycles 1.55 combines with SAMPLE at the byte level, checked with Modded-Cycles' own tools, but
  not yet tried together on a machine.
- **Not One-Shot's doing:** when loading a project asks "Save … before loading?", Yes saves the
  project you leave but does not load the other one. The official OS 1.13 does the same.

## Getting started

1. **Build the image** from your own copy of the official `model-cycles_OS1.13.syx`. You need
   Python 3, the m68k-elf toolchain (Homebrew: `m68k-elf-gcc`, `m68k-elf-binutils`) and a clone
   of [Modded-Cycles](https://github.com/18nelli18/Modded-Cycles) at a release with the mod picker
   (1.53 to 1.56: the build calls its registrar for added machines, `gen_macro.added_machine`):

   ```sh
   python3 machine/build.py --stock model-cycles_OS1.13.syx --modded-cycles path/to/Modded-Cycles
   ```

   This writes `build/model-cycles_OS1.13-sampler-v0.10.syx` and `build/sampler-tweak.json`, the
   same machine as a tweak file Modded-Cycles' own `tools/build.py` accepts, and checks every change
   against the stock bytes.
2. **Install it** over USB the way you install an official OS update. To go back, first set every
   SAMPLE track to one of the six stock machines (a project that still names a machine the official
   OS does not have can freeze its MACHINES page), then install the official 1.13 file the same way.
3. **Put samples on the +Drive.** Samples already there, for example from the Model:Samples OS,
   work as they are. For new ones open `uploader/index.html` in Chrome or Edge, press **Connect**,
   pick a folder and upload. Close Elektron Transfer first, which would hold the MIDI port.
4. **Play.** Set a track to SAMPLE, open the PRESET browser (FUNC + MACHINE) and open a sample.

If Chrome refuses MIDI to a page opened from disk, serve the folder locally and open
http://localhost:8000:

```sh
python3 -m http.server --directory uploader 8000
```

## Documentation

[TECHNICAL.md](TECHNICAL.md) explains how it works: how the parts of the OS that One-Shot touches
work, where and how it hooks in, the sound core and its measurements, the filter with its laws
and how close it comes to the Model:Samples', the USB file commands, notes on the Model:Samples
OS, and how to build, test and try it in an emulator.

## License

MIT, see [LICENSE](LICENSE). The repository holds no Elektron firmware: the build starts from your
own copy of the official file, and the Model:Samples table used for comparison is read from your
own copy of that OS and stays in `build/`. The protocol facts of the upload page come from
[ModdedTransfer](https://github.com/ktwzk/modded-transfer) (WTFPL) and Model-TG (MIT); the
machine-adding tools are Modded-Cycles' (MIT).
