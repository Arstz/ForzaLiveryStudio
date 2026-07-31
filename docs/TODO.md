Small changes:
    -With shift held snap Pen tool to 15 degree intervals relative to previous node, display hint text with degrees

Medium changes:
    -Allow for the bucket tool to fill if transparent pixel is the target and can produce a valid closed contour. If so fill the contour as usual, set all shapes color to magenta, and apply mask flag. 

Big changes:
    -Add speedpaint caching for telemetry
    -Apply proper car paint material from ingame files

Manual tasks:
    -Reorganize Setting and Options into categories, add hints for each in Settings with show hint on hover behaviour, detach to a json file for ease of editing
    -Create application icon, splash screen image and file association icon
    -Add hints for all settings, regroup options and settings
