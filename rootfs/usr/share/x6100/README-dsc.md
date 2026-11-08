# dsc_ships.bin and dsc_flags.bin: the DSC data files

Two files read by the DSC application. DSC works without them: a call
then shows the MMSI and the country from its MID, without station names
and flags.

## dsc_ships.bin

- **What:** MMSI, name, call sign and type of 71,685 ships and 917 coast
  stations, so a call shows who is calling. Read in place (`mmap`), not
  loaded into memory.
- **Origin:** the YaDDNet lists of ships
  (http://www.yaddnet.org/pages/php/mmsi_shipname_ordered.php) and of
  coast stations (http://www.yaddnet.org/pages/php/mmsi_coast.php; one
  name with "+N sites" for an MMSI serving several sites), plus 27 DSC
  training and test stations added by the author (Hany El Imam).
- **Licence:** GPL-3.0-or-later, as YaDDNet. A data file read by the
  LGPL DSC application, not linked into it.
- **Format and how to rebuild it:** see `src/dsc/make_ships_db.py`.
- **Your own copy:** `/mnt/dsc/ships.bin` on the SD card, if present, is
  read instead of this one.

## dsc_flags.bin

- **What:** 256 flags (the ISO 3166 codes of flag-icons), 32 x 24
  pixels (4:3), shown next to the country of a call. Already in the
  screen's pixel format.
- **Origin:** flag-icons 7.2.3 (https://github.com/lipis/flag-icons), the
  4:3 drawings (`flags/4x3/*.svg`) rendered and scaled to 32 x 24.
- **Licence:** MIT, as flag-icons:

> The MIT License (MIT)
>
> Copyright (c) 2013 Panayiotis Lipiridis
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of
> this software and associated documentation files (the "Software"), to deal in
> the Software without restriction, including without limitation the rights to
> use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
> the Software, and to permit persons to whom the Software is furnished to do so,
> subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
> FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
> COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
> IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
> CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
- **Format and how to rebuild it:** see `src/dsc/make_flags_db.py`.
