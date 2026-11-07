# SPDX-License-Identifier: LGPL-2.1-or-later
# DSC decoder test tools, built on a computer (Linux, macOS):
#   make            dsc_wav, dsc_gen
#   make test       synthetic calls at several SNR
DSC = ../../src/dsc
CFLAGS = -O2 -Wall -I$(DSC)

all: dsc_wav dsc_gen

dsc_wav: dsc_wav.c $(DSC)/dsc_decoder.c $(DSC)/dsc_format.c $(DSC)/dsc_mid.c
	$(CC) $(CFLAGS) -o $@ dsc_wav.c $(DSC)/dsc_decoder.c $(DSC)/dsc_format.c $(DSC)/dsc_mid.c -lm

dsc_gen: dsc_gen.c
	$(CC) $(CFLAGS) -o $@ dsc_gen.c -lm

test: all
	./run_tests.py

clean:
	rm -f dsc_wav dsc_gen
