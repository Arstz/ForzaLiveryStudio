Small changes:
    -Turn on privacy policy for public build

Medium changes:
    -Rename fh6_core lib to fls_core in cmake lists, unify decoder between fh6 and fm23, merge docs for cgroup and clivery

Big changes:
    -Render BigThumb.webp for livery export from the car model with textures applied
    -Retire legacy format support in next version
    -Finish livery export and re-enable it in the UI (container/retarget done + in-game
     confirmed; artwork synthesis pending; in-app export is currently gated off with an
     error popup — see docs/LIVERY_ENCODER.md)

Manual tasks:
    -Rename logos and cars to align with the game
