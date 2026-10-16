# X6100 LVGL GUI

This is part of an alternative firmware for X6100 using the LVGL library

## Installing

Open [Releases](https://github.com/gdyuldin/x6100_gui/releases/latest) page and download `sdcard.img` file (in Assets section). With balenaEtcher or Rufus
burn img file to microSD card. Insert card to the transceiver and boot it.

## Importing ADI log

Application could mark already worked callsign in the UI.
To load information about previous QSOs - copy your ADI log to the `DATA` partition and rename it to `incoming_log.adi`.
Application will import records to own log on the next boot and will rename `incoming_log.adi` to `incoming_log.adi.bak`.

*Note*: `DATA` partition will be created after first launch transceiver with inserted SD card.


## Exporting ADI log

Application stores FT8/FT4 QSOs to the `ft_log.adi` file on the `DATA` partition of SD card. This file might be used to load QSOs to online log.


## Building


* Clone repositories

```
mkdir x6100
cd x6100
git clone https://github.com/gdyuldin/AetherX6100Buildroot
git clone https://github.com/gdyuldin/x6100_gui
```

* Build buildroot

```
cd AetherX6100Buildroot
git submodule init
git submodule update
./br_config.sh
cd build
make
cd ../..
```

* Build app

```
cd x6100_gui
git submodule init
git submodule update
cd buildroot
./build.sh
```

Images built from this repository include JS8's engine, which changes their
licence: see [License](#license).

## License

The GUI's own code is LGPL-2.1-or-later ([LICENSE](LICENSE)), and every file
keeps the licence it has.

`third-party/js8core`, the JS8 engine from
[JS8Call-improved/Android-port](https://github.com/JS8Call-improved/Android-port),
is GPL-3.0 ([third-party/js8core/LICENSE](third-party/js8core/LICENSE)).
JS8 is part of the default build, so the GUI program (`x6100_gui`) built
from this repository combines the two and is distributed, as a whole, under
GPL-3.0. The other software in a firmware image (the Linux kernel, the
libraries) keeps its own licence. js8core brings two small libraries with it,
Eigen (MPL-2.0) and CRC++ (BSD-3-Clause), with their licence files in
`third-party/js8core/vendor/`.

The corresponding source of an image is:

- this repository at the tag or commit the image was built from;
- [AetherX6100Buildroot](https://github.com/OpenX6100/AetherX6100Buildroot)
  at the commit its build used (the Linux system around the GUI);
- Buildroot's `legal-info` for that build (the licences and sources of the
  Linux packages), published with the image.

What was changed in js8core, and where it came from, is in
[third-party/js8core/UPSTREAM.md](third-party/js8core/UPSTREAM.md).
