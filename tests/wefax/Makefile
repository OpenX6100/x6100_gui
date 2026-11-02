# SPDX-License-Identifier: LGPL-2.1-or-later
# WEFAX decoder test tool, built on a computer (Linux, macOS):
#   make            wefax_wav, wefax_gen
WEFAX = ../../src/wefax
CFLAGS = -O2 -Wall -I$(WEFAX)

all: wefax_wav wefax_gen wefax_pair

wefax_pair: wefax_gen.c $(WEFAX)/decoder.c
	$(CC) $(CFLAGS) -DWEFAX_PAIR -o $@ wefax_gen.c $(WEFAX)/decoder.c -lm


wefax_gen: wefax_gen.c
	$(CC) $(CFLAGS) -o $@ wefax_gen.c -lm

wefax_wav: wefax_wav.c $(WEFAX)/decoder.c
	$(CC) $(CFLAGS) -o $@ wefax_wav.c $(WEFAX)/decoder.c -lm

clean:
	rm -f wefax_wav wefax_gen wefax_pair
