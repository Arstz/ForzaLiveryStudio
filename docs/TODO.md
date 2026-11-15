Small changes:
    -Add toggle for selection outline
    -Anull UV cache on manual model change
    -Deny export to games with no car model
    -Change car model with missing install should say "No model found within configured games folders (expected one of %gamesSupported), continue without?"
    -Target car in Header should say "%carName (No model found)" and paint orange only in that case
    -Add toggle for wireframe in 2D canvas only (Group with show car UV unwrap to Option -> UV ->)
    -Fix status bar height to 32 px
    -Write "Shape Name (decimal_ID)" in Layers widget on new layer created

Medium changes:
    -Discard shapes/logos per version
    -On import prompt missing model, provide closest match
    -Wire back LOD and Parts, unify with PAINT MENU in 3D preview

Big changes:
    -Add support for xb360 titles (.liv)

Manual tasks:
    -Create splash screen image