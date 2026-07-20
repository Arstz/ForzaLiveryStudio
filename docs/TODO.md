Small changes:
    -Cannot copy guide layer to another section, results in empty layer created
    -Apply last shape color flag does not work [fixed on main branch]
    -Add --parrallel [threads] to build.ps1, default on parrallel 4

Medium changes:
    -Add a checkbox to allow Opacity and Skew tools to be separate (needs 2 new icons), if checked no skew anchor should appear in transform tool, off by default.

Big changes:
    -Unify all key bindings to propagate from single place, needs a proper event system to not consume a key twice in different windows, ensure no hardcoded bindings remain
    -Apply proper car paint material from ingame files
    -Retire legacy format support in next version

Manual tasks:
    -Add hints for all settings, regroup options and settings
    -Rename logos and cars to align with the game
