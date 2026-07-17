Small changes:
    -Add "Save as" button on Control Shift S
    -Change debug binding from P to U in 3d model view
    -Car model not updated on Target car changed

Medium changes:
    -Save car color metadata on project save if imported from livery

Big changes:
    -Render BigThumb.webp for livery export from the car model with textures applied
    -Retire legacy format support in next version
    -Finish livery export and re-enable it in the UI (container/retarget done + in-game
     confirmed; artwork synthesis pending; in-app export is currently gated off with an
     error popup — see docs/LIVERY_ENCODER.md)

Manual tasks:
    -Rename logos and cars to align with the game
