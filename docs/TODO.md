Small changes:
    -Enable "Show car UV unwrap" by default on livery project loaded.
    -Load textures and model in the background to not stall the main thread 
    -Unify material hashes in one file
    -Enforce do not load external textures and materials if "Load car textures" not ticked
    -Fallback to loading GUIEditor\cpp-port\assets\raster\MissingTexture.png instead of default grey paint on texture loading fail

Medium changes:
    -Make Header widget fields reactive, remove Project menu item and its buttons, Header should be the only source of truth for the project metadata, needs adding target car label with "Change" button  

Big changes:
    -Add proper scaffolding and header generation from scratch for liveries
    -Retire legacy format support in next version
    -Add tuning details support for liveries
    -Add loading support for default forza liveries

Manual tasks:
    -Reorganize Setting and Options into categories, add hints for each in Settings with show hint on hover behaviour, detach to a json file for ease of editing
    -Create application icon, splash screen image and file association icon
    -Rewrite manual user-oriented
    -Add hints for all settings, regroup options and settings
    -Rename logos and cars to align with the game
