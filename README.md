# nmemu - Clavia Nord Micro Modular emulator

An emulation of the Clavia Nord Micro Modular (1998), built on the CPU cores of
[gearmulator](https://github.com/dsp56300/gearmulator): the MC68331 host runs the original
OS, a DSP56303 runs the original DSP kernel and the module code the OS links into it. Patches
are loaded the way the editors do it, through the synth's own PC port protocol, so what you hear
is the original firmware doing the work.

Available as CLAP, VST3 and AU plugins, a standalone app, and a console renderer. The plugin
can open the [Animatek NME](https://github.com/animatek/Animatek-NME) patch editor, connected
to the emulated synth through a virtual MIDI port, so the emulation can be patched like the
hardware.

## Getting started

### 1. Get the Nord Micro Modular OS

The emulator does not include Clavia's firmware. You need the Micro Modular OS as Clavia
published it, the Windows updater `MicroModularUpdate303b.exe` (OS 3.03). The plugin extracts and
descrambles the OS from that file by itself. Put the file into one of these folders:

- `~/Documents/nmemu`
- `~/Library/Application Support/nmemu`
- the folder the plugin or app lives in
- any folder named by the `NMEMU_ROM_DIR` environment variable

A boot flash dump is not needed. On the first run the emulator formats its own patch memory and
saves it as `micromodular_flash.bin` next to the OS file, so patches you store in the synth survive.

### 2. Install

Download `NordMicroModular-<version>-macOS.zip` from the
[releases page](https://github.com/helica1/nmemu/releases) and copy

| bundle | into |
|---|---|
| `NordMicroModular.clap` | `~/Library/Audio/Plug-Ins/CLAP/` |
| `NordMicroModular.vst3` | `~/Library/Audio/Plug-Ins/VST3/` |
| `NordMicroModular.component` | `~/Library/Audio/Plug-Ins/Components/` |
| `NordMicroModular.app` | anywhere, for example `/Applications` |

The binaries are not signed or notarized. macOS will refuse to load them until the quarantine
flag is removed:

```
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/NordMicroModular.clap
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/NordMicroModular.vst3
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/NordMicroModular.component
xattr -dr com.apple.quarantine /Applications/NordMicroModular.app
```

Logic and GarageBand only see the AU after a restart, or after `killall -9 AudioComponentRegistrar`.

The release build is for Apple Silicon (arm64) Macs running macOS 11 or later. Intel Macs and
Windows/Linux need a build from source, see below.

### 3. Play

The synth boots its OS the moment the plugin loads, that takes well under a second. Then:

- **Load patch (.pch)** uploads a Clavia patch file. The 3.0 format and the older 2.10 and 1.10
  text formats are understood. 2Output modules that use outputs 3/4 are pointed at 1/2, a Micro
  Modular has no outputs 3/4.
- **Open editor** opens the Animatek NME editor in its own window, already connected to the
  emulator. Everything the editor does, patching, parameter changes, storing patches in the synth,
  goes through the real protocol. Any other Nord Modular editor (nomad, ...) can connect to the
  virtual MIDI port `Nord Micro Modular (emulator)` instead.
- The four knobs are the front panel's master level and the three assignable knobs, the fifth is
  an output gain for the plugin (the hardware's line level is quiet by DAW standards, +18 dB is
  the default).
- MIDI notes and controllers from the host go to the synth's MIDI IN. Sysex from the host goes to
  the PC port, so patch dumps and editor traffic can also come from the DAW.

A patch that stays silent usually waits for something: many archive patches expect audio at the
input, a clock or on/off switch that is stored off and assigned to a knob, or a morph or MIDI
controller to open a level. See `tools/pchinfo.py` to print what a patch is wired to.

### Console renderer

```
build/nmmConsole/nmmConsole --os MicroModularUpdate303b.exe --dsp 0 \
    --seconds 8 --pch 0.2:patch.pch --note 1.0:48:2.5 --note 4.0:60:2.5 --wav out.wav
```

`--note <sec>:<note>[:<duration>]` schedules MIDI notes, `--midi-clock <bpm>` sends MIDI clock,
`--convert in.pch out.pch` rewrites any supported patch file as 3.0 text.
`tools/renderall.py <folder> <outdir>` renders a whole patch archive to MP3 in parallel,
`tools/batch.py` renders a random sample and reports which patches stay silent and why.

## Building from source (macOS)

```
git clone --recursive https://github.com/helica1/nmemu
git clone --branch nord-modular --recursive https://github.com/helica1/gearmulator   # next to nmemu
cd nmemu && mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
ninja nmmConsole nmmPlugin_All nmmPlugin_CLAP
```

The plugins land in `build/nmmPlugin/nmmPlugin_artefacts/Release`. `-DNMEMU_EDITOR=OFF` builds
without the embedded editor (and then uses gearmulator's JUCE 7 instead of the editor's JUCE 8),
`-DNMEMU_FORCE_INTERPRETER=ON` builds the DSP interpreter instead of the JIT (what iOS will run),
`-DNMEMU_BUILD_STANDALONE=ON` adds the standalone app.

The emulation relies on fixes in the forked `dsp56300` core (branch `nord-modular-fixes`), which
the gearmulator fork's `nord-modular` branch references. Release `v0.1.0` of nmemu builds against
the `nmemu-v0.1.0` tags of both forks and of the `Animatek-NME` fork (branch `nmemu-host`).

## What works and what does not (v0.1.0)

- One Micro Modular: one DSP, four voices, outputs 1/2, MIDI in, PC port. The full Nord Modular
  with four DSPs is not emulated.
- The DSP is run either by a JIT (fast, the default) or by an interpreter. Both produce the same
  results on everything that was compared; the emulation has been checked against a few thousand
  archive patches, not against hardware recordings.
- No audio input into the synth yet, so vocoder and effect patches stay silent.
- Knob and MIDI controller assignments are not shown in the plugin's own UI, use the editor.
- Windows and Linux builds are untested.

## Layout

- `nmmLib` - the machine: MC68331 memory map, DSP56303 host port, flash, PC port DUART, front
  panel, real-time pacing (`nmmhardware`), `synthLib::Device` adapter, `.pch` parser, writer and
  sysex upload (`nmmpatch`), module table generated from nmedit's `modules.xml` (`nmmmoduledb`)
- `nmmPlugin` - JUCE plugin, virtual MIDI port, embedded editor window
- `nmmEditor` - the Animatek NME sources as a library
- `nmmConsole` - command line renderer and bring-up tool
- `nmmDspTest` - DSP core unit tests, a disassembler and an interpreter/JIT differential harness
- `nmmMidiTest` - talks to a running emulator through its virtual MIDI port like an editor
- `tools` - batch rendering, patch inspection and test patch generators

## License

GPL v3, see `LICENSE`, like gearmulator, nmedit and Animatek NME whose code and data this project
builds on. The Nord Modular OS is Clavia's and is not part of this project.
