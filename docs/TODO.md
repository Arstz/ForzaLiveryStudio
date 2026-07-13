Small changes:
    -Privacy policy enabled, still can open locked livery and groups
    -Hex search by id breaks GUI in shapes widget, ignore 0x towards required length to compensate, so user should input at least 0x + 3 symbols if starts with 0x 
    -Not all shapes copied via ALT+Drag in move and tranform tools, copied shapes should not be inserted into same parent, but a sibling instead. 

Big changes:
    -Render BigThumb.webp for livery export from the car model with textures applied
    -Retire legacy format support in next version
    -Finish livery export and re-enable it in the UI (container/retarget done + in-game
     confirmed; artwork synthesis pending; in-app export is currently gated off with an
     error popup — see docs/LIVERY_ENCODER.md)
    -Use a custom explorer when oppening groups so the user can read the Creator, Group name and preview.
    -Pixel based editor widget that will allow generating pixel graphics directly from raster with meshing algorithm via squares

Manual tasks:
    -Rename logos and cars to align with the game
