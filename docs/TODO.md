Small changes:
    -Cannot copy guide layer to another section, results in empty layer created
    -Apply last shape color flag does not work [fixed on main branch], review changes on commit 4403f4e and adapt to refactored code on this branch
    -Mixed selection can mutate bounding box on flip
    -Changing sections takes time, should be cached, worked before refactor
    -Add a toggle in options "Allow move outside bounding box", on by default, allows user to move shapes in move and transform tools even outside BB, if "move tool auto-select" turned on, prefer to move current shapes instead of overriding the selection
    -Switch from Pipette tool to last used tool on color picked
    -Apply buffer and layer background as optional dropdown list, mimic "Dark canvas" and "Light canvas" options with either "Theme default", "Checkerboard" or "Custom". Ensure the values actually propagate to both, as they are not theme aware as of now.
    -Grey-out "Published" in Header widget to indicate read-only clearly
    -Add arithmetic operation evaluation in Properties widget on direct value input
    -Override Tab key globally to deny default QT navigation (currently only applied in Settings widget), also override UI confirm so no accidental action is produced.
    -Ensure the section divider positions in both Shapes and Layers widget are saved to QSettings and loaded
    -Allow the Toolbar widget to be vertical style with icons only, Photoshop style, add as a switch in Settings

Medium changes:
    -Itegrate contour created via pen/lining/bucket actions: point created, moved, deleted. etc... as proper history actions
    -Handle files that were passed as params on application launch, if 3so -> open project if image pass the image as a Guidelayer to a new project
    -Add a checkbox to allow Opacity and Skew tools to be separate (use ToolOpacity and ToolbarSkew), if checked no skew anchor should appear in Transform tool, off by default.
    -Add dragging layer in Layers widget to scroll the list if near upper or lower borders

Big changes:
    -Add proper scaffolding and header generation from scratch for liveries
    -Apply proper car paint material from ingame files
    -Reorganize Setting and Options into categories, add hints for each in Settings with show hint on hover behaviour, detach to a json file for ease of editing
    -Retire legacy format support in next version

Manual tasks:
    -Create application icon, splash screen image and file association icon
    -Rewrite manual user-oriented
    -Add hints for all settings, regroup options and settings
    -Rename logos and cars to align with the game
