#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Builds bin/wefax_pair.exe and bin/wefax_wav.exe for Windows on Linux or a
# Mac, with zig as cross compiler (pip install ziglang). Run again after a
# change of src/wefax/decoder.c or of the tools.
set -e
cd "$(dirname "$0")"
W=../../../src/wefax
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -s -DWEFAX_PAIR -I$W -o bin/wefax_pair.exe ../wefax_gen.c $W/decoder.c
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -s -I$W -o bin/wefax_wav.exe ../wefax_wav.c $W/decoder.c
rm -f bin/*.pdb
ls -l bin
