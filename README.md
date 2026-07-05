# Forza Livery Studio

I ~hate~ love this name. A standalone C++ QT editor for Forza vinyl groups and in the future *probably* for liveries. **Does not** modify the game memory in runtime. We are not responsible for any damage done to your groups/liveries, use at your own discretion.

## Features

- Import/export to Forza proprietary binary format.
- Save/load project to json files.
- Full transformations for shapes and groups.
- Custom groups, reusable in multiple projects.
- Add raster image overlay as guide layer.
- Direct shape parity with the game engine.
- Blazingly fast perfomance thanks to C++.

## Usage

Download the latest release, launch `ForzaLiveryStudio.exe`. Read [manual](docs/MANUAL.md) for keyboard shortcuts and tools guide. 

Configure all the windows as you need them and press `Window -> Save Layout`. If you want to rename the default shapes, go to `assets/vector/shape_names.json`. 

All settings as well as custom groups are stored in your QSettings, in the registry `HKEY_CURRENT_USER\Software\ForzaTools\ForzaLiveryStudio`.

## Building

### Windows

Requirements: Qt6 via vcpkg, C++ compiler.

Run the build script:
```powershell
.\tools\build.ps1
```

See [developer guide](docs/DEV.md) for detailed instructions.

### Linux (Arch)

Requirements: Qt6, zlib (available from pacman), CMake 3.24+, C++20 compiler.

Install dependencies:
```bash
sudo pacman -S qt6-base qt6-tools qt6-svg zlib cmake gcc ninja
```

Configure and build:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run:
```bash
./build/ForzaLiveryStudio
```

See [developer guide](docs/DEV.md) for detailed build and development instructions.

## Status

The import/export for groups is fully supported, core functionality in place. The liveries can be only imported. The icons are handmade, we need a proper designer, I know they are ugly but at least we wont get sued. The application targets Forza games generally; compatibility may still vary by title because not every game/version has been verified.

## Documentation

- [**User Manual**](docs/MANUAL.md) - keyboard shortcuts, tools, panels, and recommended pipelines.
- [**Developer Guide**](docs/DEV.md) - build steps, repository layout, code map, and core entry points.
- [**C_group Binary Format**](docs/CGROUP.md) - technical reference for the vinyl group binary payload.
- [**C_livery Binary Format**](docs/CLIVERY.md) - technical reference for the livery binary container and embedded groups.
- [**Header File Format**](docs/HEADER.md) - structure of the Forza Horizon header metadata file.
- [**TODO**](docs/TODO.md) - planned features and improvements.

## Q/A

- Did you use AI? - Yes. Initially all of project base has been built in python by hands, but due to performance limitations we decided to port the project to C++. To speed up porting we used AI.
- When `[FeatureName]`? - Tomorrow.
- Can I get banned for this? - No.
- I want `[FeatureName]`, where to request? - Create an issue in this repo.

## Credits

- [Fr4g3z](https://github.com/Fr4g3z) - cool guy, helped a lot, complained a lot, format reversing.
- Mixbob - lazy bastard, tested ingame, usage feedback.
- [Zloysvin](https://github.com/Zloysvin) - shape renamer.
- [Pengyss](https://github.com/Pengyss) - non-uniform group tranform algorithm.
- Eaterrius - big money man, provided tokens.
- All the people's liveries/groups I used to decode the format.