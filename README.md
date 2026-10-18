# Cycles One-Shot

A one-shot sampler machine for the Elektron Model:Cycles, OS 1.13. It adds SAMPLE as a 7th machine
next to the six stock ones and plays samples from the +Drive. You choose a sample in the OS's own
PRESET browser, and it is saved with the sound.

The goal is a sampler for the Cycles that is small and stands on its own: no new menus, a few
separate parts instead of a firmware overhaul. Not a monolith: just the basics, ready to grow.

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
- **A waveform view.** On a SAMPLE track the main screen shows the sample's name in a bar like the
  OS's own browser title, and the waveform across the whole width. Lines mark where a trig starts
  and stops; they move while you turn Start and Length.
- **An upload page.** `uploader/` puts audio files on the +Drive over USB from Chrome or Edge, as
  samples SAMPLE plays. It needs no firmware change, so it works with the official OS as well.

| Knob | SAMPLE |
|---|---|
| PITCH (in tenths; FUNC + PITCH in semitones), the trig's note | pitch, as on every machine. PITCH 0 at note 60 plays the sample as recorded |
| COLOR | Start, 0–120 |
| SHAPE | Length, 0–120; it ends at Start + Length, at most at the sample's end. 120 (the default) plays to the end |
| SWEEP, CONTOUR | nothing yet |
| DECAY, GATE, PUNCH | the stock amp chain, set up as on TONE. DECAY starts at 127, so a sample plays out; GATE is heard with a lower DECAY |

## Status

- **Machine 0.4** (waveform view, Start and Length as on the Model:Samples, fades): tested on a
  Model:Cycles (2026-10-17), and in the digiemu emulator.
- **Upload page 0.1:** tested with a Model:Cycles (Chrome on macOS).
- **Limits:** SAMPLE cannot be combined with Model-TG or with Modded-Cycles' track filter, which
  hook the same functions of the OS.
- **Not One-Shot's doing:** when loading a project asks "Save … before loading?", Yes saves the
  project you leave but does not load the other one. The official OS 1.13 does the same.

## Getting started

1. **Build the image** from your own copy of the official `model-cycles_OS1.13.syx`. You need
   Python 3, the m68k-elf toolchain (Homebrew: `m68k-elf-gcc`, `m68k-elf-binutils`) and a clone
   of [Modded-Cycles](https://github.com/18nelli18/Modded-Cycles), whose machine-adding tools the
   build uses:

   ```sh
   python3 machine/build.py --stock model-cycles_OS1.13.syx --modded-cycles path/to/Modded-Cycles
   ```

   This writes `build/model-cycles_OS1.13-sampler-v0.4.syx` and checks every change against the
   stock bytes.
2. **Install it** over USB the way you install an official OS update. To go back, install the
   official 1.13 file the same way.
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
work, where and how it hooks in, the sound core and its measurements, the USB file commands, notes
on the Model:Samples OS, and how to build, test and try it in an emulator.

## License

MIT, see [LICENSE](LICENSE). The repository holds no Elektron firmware: the build starts from your
own copy of the official file, and the Model:Samples table used for comparison is read from your
own copy of that OS and stays in `build/`. The protocol facts of the upload page come from
[ModdedTransfer](https://github.com/ktwzk/modded-transfer) (WTFPL) and Model-TG (MIT); the
machine-adding tools are Modded-Cycles' (MIT).
