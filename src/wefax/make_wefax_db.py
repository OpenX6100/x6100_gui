#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
"""
Xiegu X6100 - WEFAX stations and schedules from a saved copy of weatherfax.com.

  wget -r -l 1 -e robots=off --reject-regex '\\.(png|jpe?g|gif|pdf|css|js|svg|webp|woff2?)' \
       https://weatherfax.com/stations/
  python3 make_wefax_db.py weatherfax.com rootfs/usr/share/x6100/wefax_stations.txt

Output (ASCII, '|' separated, one record per line):
  S|country|place|callsign|freq[:call] freq ...|note  a station (call: per-frequency callsign)
  G|freq freq ...                                       the following entries are sent on these
  E|HHMM|description                                    one broadcast (UTC)
"""
import datetime
import json
import re
import sys
import unicodedata

from bs4 import BeautifulSoup

FREQ_RE = re.compile(r'^(\d+(?:\.\d+)?)\s*kHz$', re.I)
TIME_RE = re.compile(r'^([01]\d|2[0-3])([0-5]\d)\b\s*(.*)$')
NOTE_RE = re.compile(r'^\((\d+)\)\s*(.+)$')
DATED_RE = re.compile(r'^20\d\d\s+[A-Z][a-z]{2}\b')       # "2024 Jun 10 - Oct 1": an old special schedule


def ascii_text(s):
    s = s.replace('–', '-').replace('—', '-').replace('’', "'").replace(' ', ' ')
    s = unicodedata.normalize('NFKD', s)
    s = ''.join(c for c in s if 32 <= ord(c) < 127)
    return ' '.join(s.split()).replace('|', '/')


def fnorm(f):
    return '%.1f' % float(f)


def page_lines(path):
    soup = BeautifulSoup(open(path, encoding='utf-8'), 'lxml')
    ec = soup.select_one('main .entry-content')
    for a in ec.find_all('a'):
        if a.get_text(strip=True).lower() == 'sample':
            a.decompose()
    for br in ec.find_all('br'):
        br.replace_with('\n')
    out = []
    for el in ec.find_all(['p', 'h1', 'h2', 'h3', 'h4', 'h5', 'li', 'td', 'pre']):
        if el.find_parent(['p', 'li', 'td', 'pre']):
            continue
        for l in el.get_text('').split('\n'):
            l = ascii_text(l)
            if l:
                out.append((el.name, l))
    return out


def parse_stations(root):
    soup = BeautifulSoup(open(root + '/stations/index.html', encoding='utf-8'), 'lxml')
    ec = soup.select_one('main .entry-content')
    st, country = [], None
    for el in ec.children:
        if getattr(el, 'name', None) is None:
            continue
        cls = el.get('class') or []
        if el.name == 'p' and 'has-large-font-size' in cls:
            country = ascii_text(el.get_text(' ', strip=True))
        elif el.name == 'div' and any('coblocks-row' in c for c in cls):
            cols = el.select('.wp-block-coblocks-column')
            txt = [ascii_text(c.get_text(' ', strip=True)) for c in cols]
            a = cols[1].find('a') if len(cols) > 1 else None
            fr = re.findall(r'\d+(?:\.\d+)?', re.sub(r'\s*\.\s*', '.', txt[2] if len(txt) > 2 else ''))
            st.append(dict(country=country, place=txt[0], call=(txt[1] if len(txt) > 1 else '').replace(' ', ''),
                           url=a['href'] if a else '', freqs=[fnorm(f) for f in fr]))
    return st


def parse_schedule(root, s, report):
    slug = s['url'].rstrip('/').split('/')[-1]
    lines = page_lines('%s/%s/index.html' % (root, slug))
    notes, groups, note_text = {}, [], ''
    cur, last_was_freq, stopped = None, False, False
    for tag, l in lines:
        m = NOTE_RE.match(l)
        if m:
            notes[m.group(1)] = m.group(2)
            continue
        if stopped:
            continue
        if DATED_RE.match(l):
            report.append('%s: skipped special schedule "%s"' % (s['call'], l))
            stopped = True
            continue
        m = FREQ_RE.match(l)
        if m:
            f = fnorm(m.group(1))
            if last_was_freq and cur is not None and not cur['e']:
                cur['f'].append(f)
            else:
                cur = {'f': [f], 'e': []}
                groups.append(cur)
            last_was_freq = True
            continue
        last_was_freq = False
        m = TIME_RE.match(l)
        if m:
            if cur is None:
                cur = {'f': list(s['freqs']), 'e': []}
                groups.append(cur)
            cur['e'].append([m.group(1) + m.group(2), m.group(3).strip()])
            continue
        if tag == 'p' and l.lower().startswith('operating'):
            note_text = l
            continue
        if l in ('UTC', 'PDF version', 'NOTES:') or tag in ('h2', 'h3'):
            continue
        report.append('%s: ignored line "%s"' % (s['call'], l))
    # notes "(n)" at the end of a description
    for g in groups:
        for e in g['e']:
            d = e[1]
            m = re.search(r'\s*\((\d)\)$', d)
            if m and m.group(1) in notes:
                d = d[:m.start()] + ' - ' + notes[m.group(1)]
            e[1] = d if d else '-'
    # frequencies of the list without a schedule, and schedules on unlisted frequencies
    have = {f for g in groups for f in g['f']}
    for f in s['freqs']:
        if f not in have:
            report.append('%s: %s kHz has no schedule' % (s['call'], f))
    for f in have - set(s['freqs']):
        report.append('%s: schedule for %s kHz, not in the station list' % (s['call'], f))
    return groups, note_text


# Stations whose page gives one callsign for several transmitters: callsign
# of each frequency (Deutscher Wetterdienst, Pinneberg).
FREQ_CALLS = {
    'DDH3/DDK6': {'3855.0': 'DDH3', '7880.0': 'DDK3', '13882.5': 'DDK6'},
}


COUNTRY_FIRST = ['GERMANY', 'UK', 'GREECE', 'USA']


def main():
    root, out = sys.argv[1], sys.argv[2]
    report = []
    st = parse_stations(root)
    # first the countries most heard in Europe, then the others alphabetically
    first = {c: i for i, c in enumerate(COUNTRY_FIRST)}
    st.sort(key=lambda s: (first.get(s['country'], len(first)), s['country'] if s['country'] not in first else ''))
    with open(out, 'w') as f:
        f.write('# 1KO125 WEFAX stations and schedules, from weatherfax.com (%s)\n' % datetime.date.today())
        n_e = 0
        for s in st:
            groups, note = parse_schedule(root, s, report) if s['url'] else ([], '')
            fc = FREQ_CALLS.get(s['call'], {})
            freqs = ' '.join(fr + (':' + fc[fr] if fr in fc else '') for fr in s['freqs'])
            f.write('S|%s|%s|%s|%s|%s\n' % (s['country'], s['place'], s['call'], freqs, note))
            for g in groups:
                f.write('G|%s\n' % ' '.join(g['f']))
                for t, d in g['e']:
                    f.write('E|%s|%s\n' % (t, d))
                    n_e += 1
    print('WEFAX: %d stations, %d frequencies, %d schedule entries -> %s'
          % (len(st), sum(len(s['freqs']) for s in st), n_e, out))
    for r in report:
        print('  ' + r)


if __name__ == '__main__':
    main()
