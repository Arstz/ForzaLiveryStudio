Small changes:
    -Anull UV cache on manual model change
    -Deny export to games with no car model
    -Change car model with missing install should say "No model found within configured games folders (expected one of %gamesSupported), continue without?"
    -Target car in Header should say "%carName (No model found)" and paint orange only in that case
    -Improve PAINT menu design

Medium changes:
    -Add filter for car selection menu (Game, Year, Manufacturer, Type(?); Single/Multiple)
    -Filter parts by game version (Single/Multiple), hide/paint
    -Discard shapes/logos per version
    -On import prompt missing model, provide closest match

Big changes:
    -Vulcan migration(?)
    -Proper 3D preview scene
    -Add support for xb360 titles (.liv)

Manual tasks:
    -Create splash screen image