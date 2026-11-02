# WEFAX decoder tests

A tool to run a recording through the WEFAX decoder (`src/wefax/decoder.c`)
on a computer (Linux or macOS):

    make
    ./wefax_wav REC.wav old.pgm              # phase discriminator
    ./wefax_wav REC.wav new.pgm --denoise    # Denoise ON

`wefax_gen IN.pgm OUT.wav [--snr DB] [--paths N] [--delay MS] [--doppler HZ]
[--flat S] [--clicks N] [--off HZ] [--ppm N]` makes the broadcast of a
picture (start tone, phasing, picture, stop tone) through a simulated HF
channel: noise, 2-3 propagation paths that fade on their own (selective
fading), slow fading, static crashes, line timing jitter. Tuned on 5 real
Northwood recordings (8-9 dB SNR in 2500 Hz, tone fades of a few dB, about
1200 dips of 1 ms below -10 dB a minute), a channel like them is

    wefax_gen IN.pgm OUT.wav --snr 8.5 --paths 2 --scatter -3 --delay 0.3 --doppler 0.5 --jitter 1.5

`wefax_score.py ORIGINAL DECODED.pgm`
aligns a decoded fax with its original and prints the mean gray error.

`--cont` decodes from the beginning of the recording (Manual RX) when the
start tone is not in it. The picture is written as PGM, 1809 pixels wide,
without the slant correction of the radio; any image viewer opens it.

Denoise (button on page 4 of the WEFAX window, ON by default) was tuned on
recordings of GYA Northwood and DWD: with selective fading it removes most
of the black streaks of the white areas; the sharpening that follows gives
thin lines and text the contrast of the classic receiver.
