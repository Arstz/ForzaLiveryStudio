# WGS Asset Export

WGS export registers new liveries and layer groups. Select the destination game
and its WGS container format, then choose `containers.index` from the save
profile. The package and service identifier identify the destination. The
account identifier in the profile folder supplies the creator identity. The
dialog remembers the last successful export game and each game's WGS index.

## Storage Layout

Version-14 indices register logical container names, physical container GUIDs,
active descriptor generations, timestamps, and total blob sizes. Version-4
descriptors contain fixed-size entries with UTF-16 blob names and GUID pairs.
The current GUID identifies each physical blob file. Profiles store compressed
artwork, a header, and PNG thumbnails in those files.

The selected profile determines the artwork, header, and thumbnail formats.
The export assigns fresh identifiers to the asset, folder, and blobs. Liveries
use the car ID and a timestamp in the registration name. Layer groups use the
group prefix and a timestamp.

## Transaction

The dialog asks for confirmation that the game is closed and cloud
synchronization has finished. The writer acquires a profile-level editor lock
and checks the index against the inspected version. It stages the descriptor
and blobs, backs up the original index under `.fls-wgs-backups/<backup-id>/`,
and commits the updated index last.

With the game closed, restoring the backup index against its matching save
snapshot unregisters the added asset. Check the destination game's asset browser
after exporting.
