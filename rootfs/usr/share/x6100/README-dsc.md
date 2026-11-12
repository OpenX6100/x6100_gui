# DSC: station names and flags

The DSC application is for listening only. It is not for navigation, nor
for distress and safety watchkeeping.

DSC works without any data file: a call shows the MMSI and the country
from its MID. `dsc_flags.bin` below adds the flag of that country.

## Station names: your own files

No list of ship or coast station names comes with the firmware: we know of
none whose terms allow it to be redistributed. You can add your own on the
SD card, from lists you are allowed to use:

- `/mnt/dsc/mmsi.txt`: one `MMSI;name` per line, read when DSC opens.
- `/mnt/dsc/ships.bin`: MMSI, name, call sign and type, read in place
  (`mmap`), for large lists. Built from a CSV with
  `src/dsc/make_ships_db.py`, which describes the format.

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
