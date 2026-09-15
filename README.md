# nmemu - Clavia Nord Micro Modular emulator

An emulation of the Clavia Nord Micro Modular (1998), built on the CPU cores of
[gearmulator](https://github.com/dsp56300/gearmulator): the MC68331 host runs the original
OS, a DSP56303 runs the original DSP kernel and the module code the OS links into it. Patches
are loaded the way the editors do it, through the synth's own PC port protocol.

Available as CLAP, VST3 and AU plugins and as a console renderer. The plugin can open the
[Animatek NME](https://github.com/animatek/Animatek-NME) patch editor, connected to the
emulated synth through a virtual MIDI port, so the emulation can be patched like the hardware.

## What you need

- The Micro Modular OS as published by Clavia, the Windows updater `MicroModularUpdate303b.exe`.
  The plugin extracts and descrambles the OS from it. A descrambled image works as well.
- Optionally a 512 KB boot flash dump (the rack's boot ROM works for the Micro Modular OS). Without
  it the OS is started directly in RAM.

Put them into one of

- `$NMEMU_ROM_DIR`
- `~/Documents/nmemu`
- `~/Library/Application Support/nmemu`
- the folder of the plugin

## Building (macOS)

```
git clone --recursive https://github.com/helica1/nmemu
git clone --branch nord-modular --recursive https://github.com/helica1/gearmulator   # next to nmemu
cd nmemu && mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
ninja nmmConsole nmmPlugin_All nmmPlugin_CLAP
```

The plugins land in `build/plugins/Release`. `-DNMEMU_EDITOR=OFF` builds without the embedded
editor (and then uses gearmulator's JUCE 7 instead of the editor's JUCE 8),
`-DNMEMU_FORCE_INTERPRETER=ON` builds the DSP interpreter instead of the JIT (what iOS will run),
`-DNMEMU_BUILD_STANDALONE=ON` adds a standalone app.

The emulation relies on fixes in the forked `dsp56300` core (branch `nord-modular-fixes`), which the
gearmulator fork's `nord-modular` branch references.

## Using the plugin

The synth needs about three seconds after loading to boot its OS. Then:

- **Load patch (.pch)** uploads a Clavia patch file (3.0 and the older 2.10 text formats).
  2Output modules that use outputs 3/4 are pointed at 1/2, a Micro Modular has no 3/4.
- **Open editor** opens the Animatek NME editor in its own window, already connected to the emulator.
  Any other editor (nomad, ...) can connect to the virtual MIDI port
  `Nord Micro Modular (emulator)` instead.
- The four knobs are the front panel's master level and the three assignable knobs.
- MIDI from the host goes to MIDI IN, sysex from the host goes to the PC port.

## Console

```
build/nmmConsole/nmmConsole --os MicroModularUpdate303b.exe --boot bootflash.bin --dsp 0 \
    --seconds 8 --pch 3.5:patch.pch --note 4.5:60 --wav out.wav
```

`tools/render.py <outdir> <patch.pch>...` renders a set of patches to WAV files with a note
sequence, `tools/mkpch.py` writes small test patches.

## Layout

- `nmmLib` - the machine: MC68331 memory map, DSP56303 host port, flash, PC port DUART, front
  panel, real-time pacing (`nmmhardware`), `synthLib::Device` adapter, `.pch` parser and sysex
  upload (`nmmpatch`), module table generated from nmedit's `modules.xml` (`nmmmoduledb`)
- `nmmPlugin` - JUCE plugin, virtual MIDI port, embedded editor window
- `nmmEditor` - the Animatek NME sources as a library
- `nmmConsole` - command line renderer and bring-up tool
- `nmmDspTest` - DSP core unit tests and an interpreter/JIT differential harness
- `nmmMidiTest` - talks to a running emulator through its virtual MIDI port like an editor

## License

GPL v3, like gearmulator, nmedit and Animatek NME whose code and data this project builds on.
