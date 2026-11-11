# DSC decoder tests

Tools to test the DSC decoder (`src/dsc/dsc_decoder.c`) on a computer
(Linux or macOS):

    make
    ./run_tests.py            # synthetic calls at several SNR and fadings
    ./dsc_wav REC.wav         # the calls of a recording

| Tool | What it does |
|---|---|
| `dsc_gen OUT.wav [--snr DB] [--off HZ] [--fade S] [--sfade S] [--seed N] SYMBOLS...` | a DSC call (ITU-R M.493, 100 Bd, 170 Hz, dot pattern, phasing, DX/RX, EOS and error check) as 12000 Hz audio, with noise in 2500 Hz, frequency offset, fading (`--sfade`: the two tones fade independently) |
| `dsc_wav IN.wav` | one line per call: `OK` verified, `ECC` error check right but not trusted, `BAD`; the characters, the meaning, how many were repaired or unsure |
| `run_tests.py` | random individual calls through the decoder: verified and right, verified but **WRONG** (must stay 0), both MMSIs right |

A call is verified only when the error check agrees, at most one character
was not sure (not received twice the same, and not clearly decided by the
soft bits), the check character is not that one, and the fields have
values of the right kind: the XOR check alone lets two wrong characters
cancel out.
