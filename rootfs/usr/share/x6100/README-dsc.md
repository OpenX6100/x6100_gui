# dsc_ships.bin and dsc_flags.bin: the DSC data files

Two files read by the DSC application. DSC works without them: a call
then shows the MMSI, the country from its MID and the coast stations of
`src/dsc/dsc_coast.c`, without ship names and flags.

## dsc_ships.bin

- **What:** MMSI, ship name, call sign and ship type of 71,685 vessels,
  so a call shows who is calling. Read in place (`mmap`), not loaded
  into memory.
- **Origin:** the YaDDNet MMSI / ship list
  (http://www.yaddnet.org/pages/php/mmsi_shipname_ordered.php), plus 27
  DSC training and test stations added by the author (Hany El Imam).
- **Licence:** GPL-3.0-or-later, as YaDDNet. A data file read by the
  LGPL DSC application, not linked into it.
- **Format and how to rebuild it:** see `src/dsc/make_ships_db.py`.
- **Your own copy:** `/mnt/dsc/ships.bin` on the SD card, if present, is
  read instead of this one.

## dsc_flags.bin

- **What:** 244 national flags, 36 x 24 pixels, shown next to the
  country of a call. Already in the screen's pixel format.
- **Origin:** flag-icons (https://github.com/lipis/flag-icons).
- **Licence:** MIT, as flag-icons.
- **Format and how to rebuild it:** see `src/dsc/make_flags_db.py`.
