# dsc_ships.bin and dsc_flags.bin: the DSC data files

Two files read by the DSC application. DSC works without them: a call
then shows the MMSI, the country from its MID and the coast stations of
`src/dsc/dsc_coast.c`, without ship names and flags.

## dsc_ships.bin

- **What:** MMSI, ship name, call sign and ship type of 99,782 vessels,
  so a call shows who is calling. Read in place (`mmap`), not loaded
  into memory.
- **Origin:** public vessel identification data. Public domain.
- **Format and how to rebuild it:** see `src/dsc/make_ships_db.py`.
- **Your own copy:** `/mnt/dsc/ships.bin` on the SD card, if present, is
  read instead of this one.

## dsc_flags.bin

- **What:** 244 national flags, 36 x 24 pixels, shown next to the
  country of a call. Already in the screen's pixel format.
- **Origin:** public domain.
- **Format and how to rebuild it:** see `src/dsc/make_flags_db.py`.
