# SPDX-License-Identifier: LGPL-2.1-or-later
# NAVTEX decoder test tools, built on a computer (Linux, macOS):
#   make            navtex_wav, sitorb_gen
#   make test       synthetic signals at several SNR
#   make bulletin_test   the bulletin assembler, on texts
NAV = ../../src/navtex
CFLAGS = -O2 -Wall -Wextra -I$(NAV)

all: navtex_wav sitorb_gen bulletin_test scan_sim

navtex_wav: navtex_wav.c $(NAV)/sitorb.c $(NAV)/sitorb.h
	$(CC) $(CFLAGS) -o $@ navtex_wav.c $(NAV)/sitorb.c -lm

sitorb_gen: sitorb_gen.c
	$(CC) $(CFLAGS) -o $@ sitorb_gen.c -lm

scan_sim: scan_sim.c navtex_wav.c $(NAV)/sitorb.c $(NAV)/sitorb.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ scan_sim.c $(NAV)/sitorb.c -lm

bulletin_test: bulletin_test.c $(NAV)/bulletin.c $(NAV)/bulletin.h
	$(CC) $(CFLAGS) -o $@ bulletin_test.c $(NAV)/bulletin.c

test: all
	./bulletin_test
	./run_tests.py

clean:
	rm -f navtex_wav sitorb_gen bulletin_test scan_sim *.o
