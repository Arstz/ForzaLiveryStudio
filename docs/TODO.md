Small changes:
    -State corruption on paste to empty section
    -Add toggle for selection outline
    -Anull UV cache on model change
    -Deny export to games with no car model
    -Change car model with missing install should say "No model found within configured games folders (expected one of %gamesSupported), continue without?"
    -Target car in Header should say "%carName (No model found)" and paint orange only in that case
    -Remove save layout option, proceed saving on change
    -Adjust OpenGL background to be theme aware
    -Add toggle for wireframe in 2D canvas only (Group with show car UV unwrap to Option -> UV ->)

Medium changes:
    -Discard shapes/logos per version
    -On import prompt missing model, provide closest match
    -Wire back LOD and Parts, unify with PAINT MENU in 3D preview
    -Expose theme color palette colors to be individually overridable if custom theme selected 

Big changes:
    -Color aware point snapping for Pen contours
    -Add support for xb360 titles (.liv)

Manual tasks:
    -Create splash screen image