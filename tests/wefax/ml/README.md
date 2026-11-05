# WEFAX post-processing: training kit

Trains the neural network of the WEFAX **Post Proc.** (Denoise button):
it cleans received weather faxes. It learns on original charts (Northwood
GYA, DWD, NOAA... the ones you collect: none are included here), sent
through a simulated HF channel and the decoder of the firmware. It runs on a Windows PC with an
Nvidia card (or Linux; a Mac works too, slower).

| File | What it does |
|---|---|
| `prepare_pairs.py` | every original through the simulated HF channel and the decoder of the firmware, many times: training pairs (received, original) |
| `train.py` | trains the network on the pairs, on the Nvidia card |
| `postproc.py` | cleans any fax (PNG saved by the radio, or a recording), no original needed |
| `test_real.py` | the network on the FAX SETs (real recordings + their originals): how much it improves real faxes |
| `model.py` | the network: a small U-Net (`--base 16`: 300,000 numbers), 0/1 black or white out |
| `export_model.py` | a trained network as the model file of the radio, `wefax_pp.bin` |
| `wfml.py` | shared code: pictures, alignment, scores, the C tools |
| `bin/*.exe` | the C tools for Windows: `wefax_pair` (channel + decoder), `wefax_wav` (decoder) |

## 1. Install (Windows, once)

In the **Command Prompt** (Start → `cmd`), one line at a time.

1. Python 3.12, 64-bit:

        winget install -e --id Python.Python.3.12

   then close and open the Command Prompt again.
2. The kit: download the repository from GitHub (*Code → Download ZIP* on
   https://github.com/TheMurusTeam/x6100_gui, branch `1ko125`) and copy the
   folder `tests\wefax\ml` to `C:\wefax-ml` (a short folder, not synced by
   OneDrive).
3. A private Python environment inside it, with the packages:

        cd C:\wefax-ml
        py -3.12 -m venv .venv
        .venv\Scripts\activate
        python -m pip install --upgrade pip
        pip install numpy pillow
        pip install torch --index-url https://download.pytorch.org/whl/cu128

   (`cu128`: the PyTorch build for the RTX 50xx cards.) Check:

        python -c "import torch; print(torch.cuda.get_device_name(0))"

   must print `NVIDIA GeForce RTX 5070`.

Every time you open a new Command Prompt: `cd C:\wefax-ml` and
`.venv\Scripts\activate`.

## 2. The material

- `C:\wefax-ml\originals`: the original pictures (`*.bmp`, 1720 x 1293)
  kept by `get_gya_originals.py` (subfolders are fine). The more, the
  better: every new day adds about 50.
- Charts of the US National Weather Service (Boston, New Orleans, Point
  Reyes, Kodiak, Honolulu): `python get_noaa_originals.py` downloads them
  in `originals\noaa`, as faxed. They change every few hours: run it again
  every 6 hours or so to collect more.
- Analysis charts of the DWD as faxed by DWD Hamburg (surface "Boden"
  and the sideways sea chart): `python get_dwd_originals.py` downloads
  them from the DWD open data in `originals\dwd`. The site keeps about two
  days: run it once a day.
- Charts of the DWD (PNG files of the web site, about 4400 pixels wide):
  `python dwd_to_original.py C:\path\to\pngs` makes them black and white
  at the size the radio receives them, in `originals\dwd`.
- `C:\wefax-ml\sets`: the FAX SETs, one folder each, with the recording
  `REC_*.wav` and its original `*.bmp`.

## 3. Training pairs

    python prepare_pairs.py originals pairs --per 20 --exclude sets

20 versions of every original, each through other conditions (noise,
selective fading, short dips, static crashes, line jitter). It uses all the
processor cores; each pair is about 3 MB. `--exclude sets` leaves out the
originals of the FAX SETs (found by content, whatever the file name): they
are the test, and must not be seen in the training. Run it again when new originals
arrive: only the new ones are done.

## 4. Training

    python train.py pairs

Every 1000 steps it prints the errors on originals kept out of the training
(mean gray difference from the original, 0..255; whole fax and text only)
next to the errors of the received faxes, and saves the best network in
`models\wefax_unet.pt` (`--out` for another name). The text counts 10 times
more than the rest (`--text-weight`), and half of the pieces the network
learns on are around text (`--text-crops`). `--base 24` makes a bigger
network: slower, maybe better.
Ctrl+C stops it; `python train.py pairs --resume` goes on.

## 5. Real faxes

    python test_real.py sets

Decodes every FAX SET again, cleans it with the network and prints the
error before and after. In `results\`: the cleaned faxes and, side by side,
original / received / cleaned.

## 6. Any fax, without original

    python postproc.py C:\path\to\faxes

Cleans the faxes saved by the radio (PNG), or recordings (WAV, decoded here
as on the radio). In `results\`: `NAME_ai.png` (cleaned) and
`NAME_side.png` (received and cleaned side by side).

## The C tools

`bin\wefax_pair.exe` and `bin\wefax_wav.exe` are built from
`tests/wefax/wefax_gen.c`, `wefax_wav.c` and the decoder of the firmware
(`src/wefax/decoder.c`) by `build_windows_tools.sh`. On Linux or a Mac,
`make` in `tests/wefax` builds them instead.

## 7. The model for the radio

    python export_model.py models\wefax_unet11.pt wefax_pp.bin

writes the network as the model file of the firmware
(`src/wefax/wefax_pp.c`), to be put in the firmware as
`rootfs/usr/share/x6100/wefax_pp.bin`. `tests/wefax/wefax_pp_test` runs the
same C code on a computer (`make` in `tests/wefax`):

    wefax_pp_test wefax_pp.bin IN.pgm OUT.pgm [--threads N] [--tile N]

It prints the multiply-adds and the time. Its output is the same as
`postproc.py` (at most 1 gray level of rounding).

