Medium changes:
    -Unify tranform detection in decoder, debug last transform mutations
    -Debug mask flag being ignored in liveries

Big changes:
    -Render BigThumb.webp for livery export from the car model with textures applied
    -Retire legacy format support in next version
    -Finish livery export and re-enable it in the UI (container/retarget done + in-game
     confirmed; artwork synthesis pending; in-app export is currently gated off with an
     error popup — see docs/LIVERY_ENCODER.md)
    -Pixel based editor widget that will allow generating pixel graphics directly from raster with meshing algorithm via squares

Manual tasks:
    -Rename logos and cars to align with the game
