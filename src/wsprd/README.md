# wsprd message unpacking

`wsprd_utils.c/.h` and `nhash.c/.h` are WSJT-X's `lib/wsprd` files
(GPL-3.0-or-later; Joe Taylor K1JT, Steven Franke K9AN), taken unchanged
from WSJT-X 2.7.0 (git.code.sf.net/p/wsjt/wsjtx, tag `wsjtx-2.7.0`,
commit b4f9a431). The WSPR app uses `unpk_()` to turn decoded bits into a
message, so it reads all three WSPR message types the way WSJT-X does:
type 1 (call, grid, power), type 2 (a call with a prefix or suffix, e.g.
PJ4/K1ABC, K1ABC/P) and type 3 (a hashed call with a 6 character grid,
named from calls heard before).
