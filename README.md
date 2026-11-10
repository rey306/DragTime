# DragTime

A tempo-synced half-speed effect (VST3 / AU / Standalone), written from scratch so you can change anything.

## Build

You need CMake 3.22+, Git, and a C++ compiler (Xcode on macOS, Visual Studio 2022 on Windows). JUCE 8.0.4 is downloaded automatically on the first configure.

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

The plugin is copied into your system plugin folder after the build. Rescan plugins in your DAW and look for "DragTime".

JUCE is free for personal use and for small earners under its Starter licence; check juce.com before selling a plugin.

## Build without installing anything (GitHub)

The project includes a GitHub Actions workflow that compiles the plugin for macOS (Apple Silicon and Intel) and Windows.

1. Create a free GitHub account and a new empty repository.
2. Upload the contents of this folder to it, including the hidden `.github` folder.
3. Open the repository's **Actions** tab. A "Build plugin" run starts on its own and takes roughly 5 to 10 minutes.
4. Open the finished run and download the artifact for your system.
5. Put `DragTime.vst3` in your VST3 folder and rescan in your DAW:
   - macOS: `~/Library/Audio/Plug-Ins/VST3` (the `.component` goes in `~/Library/Audio/Plug-Ins/Components`)
   - Windows: `C:\Program Files\Common Files\VST3`

On macOS the downloaded plugin is unsigned, so it will be blocked until you run this once in Terminal:

    xattr -cr ~/Library/Audio/Plug-Ins/VST3/DragTime.vst3

## Where to edit

| To change | Edit |
|---|---|
| Speed modes (add 0.6x, 0.9x, anything from 0 to 1) | `kSpeedModes` in `Source/Engine.h` |
| Loop lengths (add dotted or triplet lengths) | `kLoopLengths` in `Source/Engine.h` |
| The sound itself (crossfade shape, interpolation, filters) | `Engine::process` in `Source/Engine.h` |
| Live filter (resonance range, sweep glide time) | `svfCoeffs` and `updateControls` in `Source/Engine.h` |
| DJ knob sweep range and dead zone | top of `Engine::process` in `Source/Engine.h` |
| LFO rates and shapes | `kLfoRates`, `kLfoShapes`, `lfoShape()` in `Source/Engine.h` |
| Drive amount and curve | `kMaxDriveDb` and `updateControls` in `Source/Engine.h` |
| Parameter names, ranges, defaults | `createLayout` in `Source/PluginProcessor.cpp` |
| The interface | `createEditor` in `Source/PluginProcessor.cpp` |

Adding a speed is one line, for example `{ "0.6x", 0.6 },` and then raising the array size by one.

## Two filter sections

- **Band Low / Band High** choose which part of the input gets slowed. Everything outside the band passes through dry.
- **Filter Low Cut / High Cut / Resonance** band-pass the slowed signal itself. The cutoffs glide, so you can sweep or automate them live without clicks. The dry signal is never filtered.

## Filter extras

- **DJ Filter**: one knob. Centre is open, left closes a low-pass, right opens a high-pass. It stacks with the two cut knobs.
- **Filter LFO Depth / Rate / Shape**: a tempo-synced LFO that moves both cutoffs together, up to 3 octaves each way. With the filter fully open it behaves like an automatic DJ filter wobble; with a band-pass set it sweeps the band.
- **Drive**: tanh saturation on the slowed signal, placed before the filter so the filter can tame the added harmonics.

## Test the DSP without a DAW

    cd tests
    g++ -std=c++17 -O2 -I../Source test_engine.cpp -o test_engine && ./test_engine

## Limits

- Rates above 1x (speeding up) are not possible in this design: a live effect cannot read audio that has not arrived yet.
- "Bar" assumes 4/4.
- The interface is JUCE's generic slider panel, not a custom design.
