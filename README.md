# Forza Livery Studio

I ~hate~ love this name. A standalone C++ QT editor for Forza vinyl groups and in the future *probably* for liveries. **Does not** modify the game memory in runtime. We are not responsible for any damage done to your groups/liveries, use at your own discretion.

## Features

- Import/export to Forza proprietary binary format.
- Save/load project to json files.
- Full transformations for shapes and groups.
- Custom groups, reusable in multiple projects.
- Add raster image overlay as guide layer.
- Direct shape parity with the game engine.
- 3D car preview with live livery projection; auto-loads the matching car model.
- Blazingly fast perfomance thanks to C++.

## Usage

Download the latest [release](https://github.com/Arstz/ForzaLiveryStudio/releases/latest), launch `ForzaLiveryStudio.exe`. Read [manual](docs/MANUAL.md) for keyboard shortcuts and tools guide.

Configure all the windows as you need them and press `Window -> Save Layout`. If you want to rename the default shapes, go to `assets/vector/shape_names.json`.

All settings as well as custom groups are stored in your QSettings, in the registry `HKEY_CURRENT_USER\Software\ForzaTools\ForzaLiveryStudio`.

## Building

### Windows

Requirements: Qt6 via vcpkg, C++ compiler, zlib.

Run the build script:
```powershell
.\tools\build.ps1
```

See [developer guide](docs/DEV.md) for detailed instructions.

### Linux (Arch)

Requirements: Qt6, zlib, CMake 3.24+, C++20 compiler.

Install dependencies:

```bash
# Arch (SteamOS)
sudo pacman -S qt6-base qt6-tools qt6-svg zlib cmake gcc ninja

# Ubuntu/Debian
sudo apt install qt6-base-dev qt6-tools-dev qt6-svg-dev zlib1g-dev cmake g++ ninja-build

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qttools-devel qt6-qtsvg-devel zlib-devel cmake gcc-c++ ninja-build
```

Configure and build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run:

```bash
chmod +x ./build/ForzaLiveryStudio
./build/ForzaLiveryStudio
```

See [developer guide](docs/DEV.md) for detailed build and development instructions.

## Status

The import/export for groups is fully supported, core functionality in place. Liveries can be imported and previewed in 3D on a car model, but livery **export is not available yet** — the encoder is written and in-game confirmed for the container, but full artwork synthesis is still in progress, so the Export action refuses liveries for now. The icons are handmade, we need a proper designer, I know they are ugly but at least we wont get sued. The application targets Forza games generally; I will make a proper table once multiple title support is made, currently FH6 supports both import/export.

| Version     | Group          | Livery |
|-------------|----------------|--------|
| Horizon 6   | Import/Export  | Import |
| Motosport 23| Import         |    X   |

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
- Why not Rust? -  https://youtu.be/8ue3PXu3W8Q

## Credits

- [Fr4g3z](https://github.com/Fr4g3z) - cool guy, helped a lot, complained a lot, format reversing.
- Mixbob - lazy bastard, tested ingame, usage feedback.
- [Zloysvin](https://github.com/Zloysvin) - PR Manager / shape renamer.
- [Pengyss](https://github.com/Pengyss) - non-uniform group tranform algorithm.
- Eaterrius - big money man, provided tokens.
- [Doliman100](https://github.com/Doliman100) - reverse engineering Forza file formats and documentation.
- All the people's liveries/groups I used to decode the format.