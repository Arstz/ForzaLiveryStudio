# WGS Asset Export

WGS export registers new liveries and layer groups. Select **WGS / add new
container** for the native FH5 profile or **WGS (FH5->FM2023)** for the
Motorsport import profile. Choose `containers.index` in the destination
game's FH5 service namespace. The host package identifies the destination and
the account identifier in the profile folder supplies the creator identity.

## Storage Layout

Version-14 indices register logical container names, physical container GUIDs,
active descriptor generations, timestamps, and total blob sizes. Version-4
descriptors contain fixed-size entries with UTF-16 blob names and GUID pairs.
The current GUID identifies each physical blob file. FH5 stores its native
compressed artwork, header, and PNG thumbnails in those files.

The FH5 encoder supplies the artwork and header. The export assigns fresh
identifiers to the asset, folder, and blobs. Liveries use the car ID and a
timestamp in the registration name. Layer groups use the group prefix and a
timestamp.

## Transaction

The dialog asks for confirmation that the game is closed and cloud
synchronization has finished. The writer acquires a profile-level editor lock
and checks the index against the inspected version. It stages the descriptor
and blobs, backs up the original index under `.fls-wgs-backups/<backup-id>/`,
and commits the updated index last.

With the game closed, restoring the backup index against its matching save
snapshot unregisters the added asset. Check the destination game's asset browser
after exporting.
