#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Makes a web page that compares the cleaned faxes (*_ai.png) of two result
folders, file by file (same name in both folders).

    compare_html.py [FOLDER_A FOLDER_B] [--out compare.html]

Default folders: results_prova_v3 and results_prova_v4. The page is written
next to the two folders and links the pictures there (nothing is copied).
One picture at a time: click it (or Space) to switch A/B, arrows or the
list to go to another fax.
"""
import argparse
import glob
import html
import json
import os


PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>WEFAX compare</title>
<style>
body{margin:0;background:#222;color:#eee;font:15px system-ui,sans-serif}
#bar{position:sticky;top:0;background:#111;padding:8px 12px;display:flex;gap:12px;align-items:center;flex-wrap:wrap;z-index:1}
#tag{font-weight:bold;font-size:20px;padding:2px 14px;border-radius:6px;min-width:90px;text-align:center}
.a{background:#c62828}.b{background:#1565c0}
select,button{font-size:15px}
#img{display:block;max-width:100%;margin:0 auto;cursor:pointer;background:#fff}
#hint{color:#aaa}
</style></head><body>
<div id="bar">
 <span id="tag"></span>
 <button id="prev">&larr;</button>
 <select id="list"></select>
 <button id="next">&rarr;</button>
 <span id="hint">click the picture or Space: switch &nbsp;|&nbsp; &larr; &rarr;: other fax</span>
</div>
<img id="img" alt="">
<script>
const A = __A__, B = __B__, NAMES = __NAMES__;
let i = 0, side = 0;
const img = document.getElementById('img'), tag = document.getElementById('tag'), list = document.getElementById('list');
NAMES.forEach((n, k) => { const o = document.createElement('option'); o.value = k; o.textContent = n; list.appendChild(o); });
function show() {
  const f = side ? B : A;
  img.src = encodeURI(f + '/' + NAMES[i]);
  tag.textContent = f; tag.className = side ? 'b' : 'a';
  list.value = i;
  // load the other one too, so switching is instant
  new Image().src = encodeURI((side ? A : B) + '/' + NAMES[i]);
}
function go(d) { i = (i + d + NAMES.length) % NAMES.length; show(); }
img.onclick = () => { side = 1 - side; show(); };
document.getElementById('prev').onclick = () => go(-1);
document.getElementById('next').onclick = () => go(1);
list.onchange = () => { i = +list.value; show(); };
document.addEventListener('keydown', e => {
  if (e.key === ' ') { e.preventDefault(); side = 1 - side; show(); }
  else if (e.key === 'ArrowLeft') go(-1);
  else if (e.key === 'ArrowRight') go(1);
});
show();
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", nargs="?", default="results_prova_v3")
    ap.add_argument("b", nargs="?", default="results_prova_v4")
    ap.add_argument("--out", default="compare.html")
    args = ap.parse_args()

    names_a = {os.path.basename(p) for p in glob.glob(os.path.join(args.a, "*_ai.png"))}
    names_b = {os.path.basename(p) for p in glob.glob(os.path.join(args.b, "*_ai.png"))}
    names = sorted(names_a & names_b)
    if not names:
        raise SystemExit(f"no *_ai.png with the same name in {args.a} and {args.b}")
    for n in sorted(names_a ^ names_b):
        print("only in one folder, left out:", n)

    out_dir = os.path.dirname(os.path.abspath(args.out))
    rel = lambda d: os.path.relpath(os.path.abspath(d), out_dir).replace(os.sep, "/")
    page = (PAGE.replace("__A__", json.dumps(rel(args.a)))
                .replace("__B__", json.dumps(rel(args.b)))
                .replace("__NAMES__", json.dumps(names)))
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(page)
    print(f"{len(names)} faxes: {html.escape(args.out)}")


if __name__ == "__main__":
    main()
