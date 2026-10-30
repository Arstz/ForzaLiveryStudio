# Forza Livery Studio — User Manual

A practical guide to the editor's workflow and tools. For
build and developer notes see
[DEV.md](DEV.md).

> No shortcut guide is provided, discover them yourself by using the editor. Most shortcuts are rebindable from `Window → Settings`.

## Getting started

The prebuilt binaries should contain ForzaLiveryStudio.exe launch it and you will be presented with the main editor window. The window contais widgets (small windows) - those are draggable and dockable and detachable, try for yourself, arrange them the way you want and locate `Window->Save Layout` in menu bar at the top. This layout would be loaded on next launch if something breaks or you lose a widget do `Window->Reset Layout`.

### Project

There are 2 project types available in the editor `Group` and `Livery`. Currently editor supports only 1 project opened per instance. By default the editor creates an empty group project. Import an existing game asset via `File->Import`. Use `File->New Project` and choose `Layer Group` or `Livery`; a livery project will also ask you to choose a car.

Save your work as a `.3so` project via `File->Save Project`, and reopen it with `File->Open Project`. Autosave is configurable in `Window->Settings` and updates projects that have already been saved. Recovery copies also cover unsaved work and can be restored after an unexpected exit.

Exporting the project is done with `File->Export`, select the destination folder, game and format. Supported games are FH2–FH6, FM5–FM7 and Motorsport 2023. The export folder is named according to project metadata. If a livery fails to load ingame on an older title (endless Saving...), exit the game, delete the livery from the folder, ungroup all and export again.

Use `File->Export project to SVG` to save the project's vector shapes as separate SVG elements with flattened transforms and no groups, raster logos will be discarded. To export livery load the car model first, the SVG will match the UV unwrap.

### Metadata

Metadata is located in `Header` widget, it will be loaded from the imported assets or created by default on new project with the creator name last used. The file would be exported as a local instance, to publish you would need to re-save it ingame and publish with Forza itself. All fields are reactive, car is changed via the button.

### Shapes

To place shapes interact with `Shapes` widget. Left click would create a new shape and right would change the selected shape to the clicked one. Add and remove shapes to favourites via clicking on the star icon, shapes can be renamed by editing `assets/vector/shape_names.json` located in your install location.

To place text use `Place Text` on the toolbar, choose a font and type your text. Letters are placed as a group of shapes; `Monospace` gives them fixed spacing.

### Layers

Use `Layers` widget to select, group and reorder shapes. Drag rows to move them between groups; rows above other rows draw on top. Use `Edit->Group / Ungroup`, `Edit->Align` and `Edit->Distribute` to arrange your selection. Colors in `Swatches` are saved with the project.

### Guide layers

Using raster or SVG images as your guide is very straightforward: import one by clicking `File->Import Guide Layer` or by dropping the file into the editor window. SVG source data is kept in the project and painted as scalable vector artwork on the canvas. Pixel-based tools such as Bucket and Pipette use a temporary raster working image without replacing the SVG guide. Guide layers have opacity and can be freely transformed. You can toggle their visibility and draw order (on top/on bottom) via `Options->Guides`.

To place guidelines left click with `Alt` held on the rulers, the guidelines can be moved, deleted with right click and locked with `Options->Guides->Lock Guidelines`.

### 3D preview

3D preview is disabled by default, enable it with `Window->3D Preview`. With a Livery project open or car model loaded in a Group project, you can see the car with the decals applied. Opening a livery loads its matching car from the game folder configured in `Window->Settings`. You can also load a model with `File->Import Car Model`. Native textures can be enabled with `Load car textures` in Settings. The preview is a reference approximation of how the car looks ingame.

Rotate model with left mouse button, zoom with scrollwheel and pan with middle mouse. With model loaded you can enable UV unwrap and/or wireframe with `Options->Show Car UV Unwrap` and `Options->Show Section Wireframe`.

The connected preview side panel starts collapsed, has `PAINT`, `PARTS`, and `LOD` tabs, and can be opened or hidden with its arrow button. `PAINT` edits each supported region's primary color, secondary color, and Gloss, Semigloss, Metallic, or Matte finish. Its Body option applies color and finish changes to the hood, mirrors, and wing while `Override hood, mirrors and wing` is checked. `Clear all` removes every paint override and can be undone. Color and finish edits are saved in the project and exported livery. `PARTS` selects body kits, rear wings/spoilers, front and rear bumpers, hoods, and side skirts; compound body-kit selections switch their compatible models together. `LOD` selects the rendered model detail level. The LOD cycle command can also be assigned in `Settings->Keybinds`.

## Tools

### Select

Your main way to select shapes and groups. Select one with left click, or drag to select multiple shapes. Clicking with `Shift` selects the immediate group, repeat to move up the group tree. `Control` toggles a shape in the current selection. Selection follows the shape body; enable `Tool Settings->Pick by bounding box` to use its bounds.

### Marquee

Select multiple shapes intersecting a rectangle on left click and drag. This is part of Select by default; enable `Separate Select and Marquee tools` in `Window->Settings->Tools` to use it as a separate tool.

### Move

This tool is used, well, to move current selection. You can toggle autoselection via `Tool Settings->Auto Select`. Dragging inside the current selection moves it. With Auto Select disabled you can drag the selection from anywhere, otherwise clicking outside it can select another shape or group. `Tool Settings->Automatic snapping` aligns geometry edges and centers with nearby visible guidelines and enables contour contact and axis alignment with other visible shapes in the active section. This option is enabled by default and shared with Transform.

### Rotate

A separate tool for rotation only, if you prefer to do rotation separately or globally.

### Opacity and Skew

These tools are opt-in via `Settings` widget, used to control opacity and skew by dragging horizontally. If setting enabled it would remove separate skew anchor from the Transform tool.

### Transform

All in one tool to move, rotate, scale and skew the selection. Applying `Shift` causes the scale to be uniform, rotate to snap 15 degree intervals. Scaling with `Alt` held would pin the selection. `Tool Settings->Automatic snapping` applies here as well.

### Pipette

Click a layer to sample its color, will apply it to the current selection. You can also sample the guide layer. Every sample adds this color to Swatches widget to be reused later if not already present. The default-on `Pipette Auto-Return` option returns to the previous tool after a successful sample; disable it to keep Pipette active.

### Pen

Pen tool will place a single hard point (red) to start a contour on left click. Subsequent left clicks will place a soft point (white) on single click or hard point on double click. Contour will be completed once you close it by clicking on starting point again. `Control` + left click inside a closed contour starts an interior cutout. A closed boundary can be further refined by adding point via `Control` + left click and removed via right click. To drag an existing point hold `Alt` and left click. Snapping to 15 degree interval can be achieved while `Shift` is held.

Choose Analytical or Compact from `Tool Settings->Mode`. Pen and Bucket use the selected mode. Compact uses opaque fills and asks how far shapes may extend outside the contour. Generated shapes can be kept with a warning if the result misses its quality targets.

With a closed Pen contour in Compact mode, `Alt` + click a group in Layers to mark it as leeway. The fill can continue underneath it and will be inserted below that group.

Saved Pen and Lining paths appear in `Layers`. Use `Edit->Save Pen or Lining Path as Figure` to save the active path, then select it to resume editing. `Properties->Color` changes its outline and fill color. Each tool has its own `Tool Settings->Save Path` checkbox, off by default. Pen saves on contour closure; Lining saves when the line is finished.

>Compact fit works best on big contours with least amount of points, sometimes it might be worth to trace the contour manually

### Bucket

Bucket tool automates the process described in Pen by generating the contour on the selected guide layer within a given tolerance. Outer and interior traced boundaries remain editable when the tool changes to Pen. To adjust tolerance use scrollwheel, press left click to create a contour. Once done, your tool will be changed to Pen and you can fine-tune the contour. When satisfied confirm fill on `Enter`. Press `Escape` while Bucket is active to discard its preview and current contour. Some larger contours may take longer. On raster guides, click with `Shift` held to select only the outer contour with holes ignored. On SVG guides, Bucket selects the topmost filled vector object under the cursor and converts its authored path directly into an editable Pen contour without rasterization, curve sampling, or path simplification. Hold `Shift` while hovering or clicking an SVG guide to force the ordinary tolerance-based raster Bucket behavior instead. Live SVG text is resolved to the installed font's vector glyph outlines. SVG features that do not expose a reliable object contour, such as gradients, patterns, masks, clipping, text-on-path, and embedded images, automatically use the raster Bucket behavior instead.

>Both Bucket and Pen is not a replacement for every contour, and can have unpredictable results on finer details. For basic shapes like fonts it is still reccommended to use other means. Use both if you want to speed up the process but be aware that they might consume more shapes than if done manually.

>Clicking with Bucket tool on transparent pixels will still generate a contour but it will be filled with masked shapes instead upon confirmation.

### Lining (experimental)

Lining lets you draw an open line and fill it with shapes. New lines use black. Right-click to finish drawing, then press `Enter` to fill. `Tool Settings->Width` sets the line thickness. You can adjust the blue handles to change its width and the amber handles to change its ends. `Tool Settings->Outside margin` regulates leeway around the line. Width edits support Undo and Redo.
