Small changes:
    -Anull UV cache on manual model change
    -Deny export to games with no car model
    -Change car model with missing install should say "No model found within configured games folders (expected one of %gamesSupported), continue without?"
    -Target car in Header should say "%carName (No model found)" and paint orange only in that case

Medium changes:
    -Discard shapes/logos per version
    -On import prompt missing model, provide closest match
    -Wire back LOD and Parts, unify with PAINT MENU in 3D preview

Big changes:
    -Add support for xb360 titles (.liv)

Manual tasks:
    -Create splash screen image