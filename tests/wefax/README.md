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

## Original pictures of Northwood (GYA)

`get_gya_originals.py` keeps the original pictures of the Northwood radio fax
charts (JOMOC web site, 1720 x 1293, 1 bit), to pair them with received
faxes and to train the WEFAX denoiser. The site keeps only the latest
picture of each chart, so run it every hour; each new picture is kept under
its creation time:

    ~/x6100/gya_originals/2026-10-22/20261022-1238_HFFAX_Polar_AA_SfcAnalysis_UNCL_1200_T000.bmp

On a Mac, every hour with launchd: save as
`~/Library/LaunchAgents/net.1ko125.gya-originals.plist` (with the path of
your x6100_gui folder)

    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
    <plist version="1.0"><dict>
      <key>Label</key><string>net.1ko125.gya-originals</string>
      <key>ProgramArguments</key><array>
        <string>/usr/bin/python3</string>
        <string>/Users/YOU/x6100/x6100_gui/tests/wefax/get_gya_originals.py</string>
      </array>
      <key>StartInterval</key><integer>3600</integer>
      <key>RunAtLoad</key><true/>
      <key>StandardOutPath</key><string>/tmp/gya-originals.log</string>
      <key>StandardErrorPath</key><string>/tmp/gya-originals.log</string>
    </dict></plist>

then `launchctl load ~/Library/LaunchAgents/net.1ko125.gya-originals.plist`.
