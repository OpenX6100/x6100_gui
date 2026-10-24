# broadcast.csv: the shortwave broadcast schedule

`broadcast.csv` is the station schedule behind the Channel List's
**Broadcast Info** (microphone GENE) and **Save as channel** (NW).

## Source

The **EiBi shortwave schedule** by Eike Bierwirth, https://www.eibispace.de/
(the CSV database, `sked-<season>.csv`, unchanged).

This copy is season **A26** (summer 2026, 29 March to 24 October 2026),
from an update of that season taken for 1KO125 1.0.2 (Hany El Imam). EiBi
publishes a new file for each season: **B26** (winter) starts on
25 October 2026, then A27 at the end of March 2027, and updates each file
during its season.

## Terms of use

From EiBi's README.TXT, *Conditions of use*:

> All my frequency lists are free of cost, and any person is absolutely
> free to download, use, copy, or distribute these files or to use them
> within third-party software.

Corrections go to EiBi (the address is in that README).

## Updating it

- In the firmware: replace this file with the season's `sked-<season>.csv`
  from https://www.eibispace.de/dx/ (same format, header line included)
  and update the season above.
- On a radio: this file is read where it is, so each firmware brings its
  own season's schedule. To use another one (a newer season, or your own
  list), put it on the SD card as `/mnt/broadcast.csv` (the DATA
  partition): when that file is there, it is read instead. Delete it to go
  back to the firmware's copy.
