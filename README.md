# Forza Livery Studio
  I ~hate~ love this name. A standalone C++ QT editor for Forza vinyl groups and in the future *probably* for liveries. **Does not** modify the game memory in runtime (very cool). We are not responsible for any damage done to your groups/liveries, use at your own discretion.

  ## Features
  - Import/export to Forza proprietary binary format
  - Save/load project to json files
  - Full transformations, for groups as well
  - Custom groups, reusable for other projects
  - Raster image overlay as guide layer
  - Direct shape parity with the game engine
  - C++ (**blazingly fast**)

  ## Usage 
  Download the latest release, launch `ForzaLiveryStudio.exe`. The manual can be found in `docs/MANUAL.md`. Configure all the windows as you need them and press `Window->Save Layout`. If you want to rename the default shapes, go to `assets/vector/shape_names.json` All the settings as well as custom groups are stored in your QSettings, in the registry `HKEY_CURRENT_USER\Software\ForzaTools\ForzaLiveryStudio`

  ## Building
  Requirements: QT6 via vcpkg or another installation, reconfigure build.ps1 script if needed, any C++ compiler.

  To build run the build.ps1 script

  ## Status
  The import/export for groups is fully supported, core functionality in place. The liveries can be only imported. The icons are handmade, we need a proper designer, I know they are ugly but at least we wont get sued. The app targets Forza games generally; compatibility may still vary by title because not every game/version has been verified.

  ## Q/A
  - Is it slopcoded? -YES, the project was forced to be ported to C++ due to python performance limitations, I am not rewriting everything.
  - When `[FeatureName]`? -Tomorrow
  - Can I get banned for this? -Probably not
  - I want `[FeatureName]`, where to request? -Create an issue in this repo

  ## Credits
  - [Fr4g3z](https://github.com/Fr4g3z) - cool guy, helped a lot, complained a lot, format reversing.
  - Mixbob - lazy bastard, tested ingame, usage feedback
  - Zloysvin - shape renamer
  - [Pengyss](https://github.com/Pengyss) - non-uniform group tranform algorithm
  - Eaterrius - big money man, provided tokens
  - All the people's liveries/groups I used to decode the format
