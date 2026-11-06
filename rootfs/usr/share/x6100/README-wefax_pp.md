# wefax_pp.bin: the WEFAX Post Proc. model

`wefax_pp.bin` is the neural network of the WEFAX **Post Proc.** option
(Denoise button): every saved fax is also cleaned by it and saved next to
the original as `NAME_pp.png`; the fax as received is always kept. It is
run by `src/wefax/wefax_pp.c` and can be replaced without recompiling.

- **Author:** Hany El Imam (1KO125).
- **License:** GNU Lesser General Public License, version 2.1 or later,
  like the code (see `LICENSE`).
- **Model:** v11, U-Net, base width 16.
- **Training data:** original weather charts of Northwood (GYA) and of
  DWD Hamburg, turned into received faxes by the HF channel simulator of
  `tests/wefax/ml` (fading, noise, interference). No NOAA charts and no
  recordings were used for training; real recordings were used only to
  test it. No chart is included in the repository.
- **Training kit:** `tests/wefax/ml` (prepare the pairs, train, export to
  this file with `export_model.py`).
