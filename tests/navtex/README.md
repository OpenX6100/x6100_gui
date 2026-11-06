# NAVTEX decoder tests

Tools to test the SITOR-B receiver of the NAVTEX application
(`src/navtex/sitorb.c`) on a computer (Linux or macOS):

    make
    ./run_tests.py            # synthetic signals
    ./bulletin_test           # the bulletin assembler, on texts
    ./navtex_wav REC.wav      # a recording, any sample rate
    ./navtex_wav REC.wav --quality

| Tool | What it does |
|---|---|
| `sitorb_gen OUT.wav [--snr DB] [--off HZ] [--ppm N] [--fade S] [--sfade S] [--rate HZ] [--rev] < text` | a NAVTEX message sent in SITOR-B (phasing, DX/RX repetition, idle alpha), with noise in 2500 Hz, frequency offset, clock error, fading (`--sfade`: the two tones fade independently) |
| `navtex_wav IN.wav [--centre HZ]` | decodes a recording (16-bit PCM or 32-bit float; `--centre` when the tones are not around 1000 Hz) with the firmware decoder; resamples to 12000 Hz; `--quality` writes the quality of each character after it (0 clean, 1 from the repetition, 2 guessed, 3 lost) |
| `run_tests.py` | messages at several signal to noise ratios and fadings: character error rate |
| `bulletin_test` | the bulletin assembler (`src/navtex/bulletin.c`) on texts: NAVTEX bulletins, SITOR-B messages without ZCZC / NNNN, headers and ends lost |
| `scan_sim IN.wav [WAIT] [STEP]` | the decisions of the NAVTEX Scan on a recording: arriving on it every STEP s (default 15), after WAIT s (default 0.7) the tone test, then the lock and the text; one line per arrival with the decision |
| `navtex_sitorb_fix.py FOLDER [--dry-run]` | marks as SITOR-B messages the bulletins saved before 1KO125 knew them ("start and end lost"); see the help of the script for copying `/mnt/navtex` from and back to the radio |
