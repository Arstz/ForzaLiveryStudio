Small changes:
    -Tool buttons not resized properly after applying custom keybinds, collapsing the text
    -Skew tool hint text displays skew in px rather than editor units
    -Cursor on layer drag in Layers widget is rendered only left half visible

Medium changes:
    -Handle files that were passed as params on application launch, if 3so -> open project if image pass the image as a Guidelayer to a new project
    -Add dragging layer in Layers widget to scroll the list if near upper or lower borders

Big changes:
    -Add proper scaffolding and header generation from scratch for liveries
    -Apply proper car paint material from ingame files
    -Retire legacy format support in next version

Manual tasks:
    -Reorganize Setting and Options into categories, add hints for each in Settings with show hint on hover behaviour, detach to a json file for ease of editing
    -Create application icon, splash screen image and file association icon
    -Rewrite manual user-oriented
    -Add hints for all settings, regroup options and settings
    -Rename logos and cars to align with the game
