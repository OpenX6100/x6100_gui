# DSC: station names and flags

The DSC application is for listening only. It is not for navigation, nor
for distress and safety watchkeeping.

Without data files a call shows the MMSI and the country from its MID.
`dsc_ships.bin` below adds the name of the station, `dsc_flags.bin` the
flag of the country.

## dsc_ships.bin

- **What:** 272,403 ship and coast stations: MMSI, name, and where known
  call sign and type, shown with a call. Read in place (`mmap`).
- **Origin:** `lista.csv` of https://github.com/TheMurusTeam/dsc_db,
  turned into this file by `src/dsc/make_ships_db.py`. Every line of
  `lista.csv` names its source: FCC ULS, NOAA AIS (MarineCadastre.gov),
  Wikidata, the Danish AIS system (DEMA), Fintraffic Digitraffic and the
  ITU list of coast stations (as included in SDRangel). See its README
  for the details of each.
- **Licence:** GPL-3.0-or-later for the list, as dsc_db; the data keeps the
  terms of its source:
  - Fintraffic Digitraffic, CC BY 4.0: "Source: Fintraffic /
    digitraffic.fi, license CC 4.0 BY".
  - "Historical AIS data taken from the Danish AIS system under the Danish
    Emergency Management Agency."
  - NOAA Office for Coastal Management, Nationwide Automatic
    Identification System, Marine Cadastre, https://marinecadastre.gov
    (U.S. Government material), and FCC ULS (U.S. Government Works).
  - Wikidata, CC0.
  - The coast stations: ITU, https://www.itu.int/mmsapp/coaststation/list.

For listening only: the list may be incomplete, out of date or wrong.

## Station names: your own files

You can add your own on the SD card; they come before the firmware's list:

- `/mnt/dsc/mmsi.txt`: one `MMSI;name` per line, read when DSC opens.
- `/mnt/dsc/ships.bin`: read in place of `dsc_ships.bin`. Built from a CSV
  with `src/dsc/make_ships_db.py`, which describes the format.

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
