Small changes:
    -If a car does not have a UV section associated do not tich "Show UV unwrap to false"
    -Front window needs transpose horizontal->vertical in canvas render, both shapes and UV overlay
    -Deny UV mask leak to other side Right/Left, Back/Front
    -Override Tab in QT navigation to be elidgeable for binding in Settings widget
    -Shape retains visuals after manual ShapeID change in Properties widget
    -Define arbitrary colour values in car_renderer as variables 

Medium changes:
    -Unify tranform detection in decoder
    -Debug mask flag being ignored in liveries

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
