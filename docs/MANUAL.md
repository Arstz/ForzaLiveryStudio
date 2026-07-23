# Forza Livery Studio — User Manual

A practical guide to the editor's workflow and tools. For
build and developer notes see
[DEV.md](DEV.md).

> No shortcut guide is provided, discover them yourself by using the editor. Most shortcuts are rebindable from `Window → Settings`.

## Getting started

The prebuilt binaries should contain ForzaLiveryStudio.exe launch it and you will be presented with the main editor window. The window contais widgets (small windows) - those are draggable and dockable and detachable, try for yourself, arrange them the way you want and locate `Window->Save Layout` in menu bar at the top. This layout would be loaded on next launch if something breaks or you lose a widget do `Window->Reset Layout`.

### Project

There are 2 project types avaliable in the editor `Group` and `Livery`. Currently editor supports only 1 project opened per instance. By default the editor creates an empty group project. Importing an existing file from supported titles can be done via `File->Import`, this will  To create a livery you would need to press `File->New Project`, this would create a new project based on the asset oppened if group->group project, if livery->livery project. Exporting the project is done with `File->Export`, select a folder you wish to save into the folder will be named according to project metadata configured. I reccommend saving to ContainersRoot directory directly. The editor will export to Forza Horizon 6 format as the only supported, if support for other games is needed it is possible on request, contact the dev team on Github or Discord.

### Metadata 

Metadata is located in `Header` widget, it will be loaded from the imported assets or created by default on new project with the creator name last used. The file would be exported as a local instance, to publish you would need to resave it ingame and publish with Forza itself.

### Shapes

To place shapes interact with `Shapes` widget. Left click would create a new shape and right would change the selected shape to the clicked one. Add and remove shapes to favourites via clicking on the star icon, shapes can be renamed by editing `assets/vector/shape_names.json` located in your install location.

### Guide layers

Using raster images as your guide is very straightforward, you can import one to your project by clicking `File->Import Guide Layer` or dropping the file into editor window directly. Guide layers have opacity and can be freely transformed. You can toggle their visibility and draw order (on top/on bottom) via `Option->Guides`.

## Tools

### Select

Your main way to select shapes and groups. Select one with left click and cycle through if multiple are at the same spot by repeating. Clicking with `Shift` would select the topmost group, and `Control` would toggle a specific shape to current selection.

### Marquee

Select multiple shapes contained within a rectangle on left click and drag. Shapes will be selected based on their center point. Is global so it would ignore shape nesting into groups.

### Move

This tool is used, well, to move current selection. You can toggle autoselection via `Options`, otherwise you would have to modify selection with other tools.

### Rotate

A separate tool for rotation only, if you prefer to do rotation separately or globally.

### Opacity and Skew

These tools are opt-in via `Settings` widget, used to control opacity and skew by dragging horizontally. If setting enabled it would remove separate skew anchor from the Transform tool.  

### Transform

All in one tool to move, rotate, scale and skew the selection. Applying `Shift` causes the scale to be uniform, rotate to snap 15 degree intervals. Scaling with `Alt` held would pin the selection.

### Pipette

Click a layer to sample its color, will apply it to the current selection. You can also sample the guide layer. Every sample adds this color to Swatches widget to be reused later if not already present.

### Pen

Pen tool will place a single hard point (red) to start a contour on left click. Subsequent left clicks will place a soft point (white) on single click or hard point on double click. Contour will be completed once you close it by clicking on starting point again. A closed contour can be further refined by adding point via `Control` + left click and removed via right click. To drag an existing point hold `Alt` and left click. 

### Bucket

Bucket tool automates the process described in Pen by generating the contour on the selected guide layer within a given tolerance. To adjust tolerance use scrollwheel, press left click to create a contour. Once done, your tool will be changed to Pen and you can fine-tune the contour. When satisfied confirm fill on `Enter`. Some larger contours may take longer. For best results try to maximize tolerance and hard point count.

>Both Bucket and Pen is not a replacement for every contour, and can have unpredictable results on finer details. For basic shapes like fonts it is still reccommended to use other means. Use both if you want to speed up the process but be aware that they might consume more shapes than if done manually.

### Lining (experimental)

This tool is aimed to help tracing the finer details, same as Pen but allows for a non-closed contour, to stop drawing press right mouse button. Adjust width with scrollwheel, it gives the filling algorithm some leeway, confirm with `Enter` it will try to match the curve with shapes, try to maximize hardpoints and minimize contour lenght. Try different width settings, usually best around 4-8.
