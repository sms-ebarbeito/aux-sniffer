#!/usr/bin/env python3
"""DisplayPort AUX decoder for sigrok .sr captures (receiver output on one channel)."""
import zipfile, sys, argparse
import numpy as np

def load(path, ch):
    z = zipfile.ZipFile(path)
    meta = z.read('metadata').decode()
    sr = 24e6
    for l in meta.splitlines():
        if l.startswith('samplerate='):
            v, u = l.split('=')[1].split()
            sr = float(v) * {'Hz': 1, 'kHz': 1e3, 'MHz': 1e6}[u]
    names = sorted([n for n in z.namelist() if n.startswith('logic-1')], key=lambda n: int(n.split('-')[-1]))
    d = np.frombuffer(b''.join(z.read(n) for n in names), dtype=np.uint8)
    b = (d >> ch) & 1
    idx = np.flatnonzero(np.diff(b)) + 1
    return b, idx, sr

def frames(b, idx, sr, gap_us=6.0):
    """Split edges into frames; return list of (t_start, runs[(level, units)])."""
    if len(idx) == 0:
        return
    t = idx / sr * 1e6
    br = np.flatnonzero(np.diff(t) > gap_us)
    starts = np.r_[0, br + 1]; ends = np.r_[br, len(t) - 1]
    for s, e in zip(starts, ends):
        if e - s < 20:
            continue
        runs = []
        for k in range(s, e):
            lvl = b[idx[k]]
            u = int(round((t[k + 1] - t[k]) / 0.5))
            if u > 0:
                runs.append((lvl, u))
        # last level after final edge: assume it's stop low/idle, add 4 units
        runs.append((b[idx[e]], 4))
        yield t[s], runs

def halfbits(runs):
    out = []
    for lvl, u in runs:
        out += [int(lvl)] * min(u, 12)
    return out

def decode_frame(hb, pol, bitmap):
    syncpat = [pol] * 4 + [1 - pol] * 4
    s = ''.join(map(str, hb)); p = ''.join(map(str, syncpat))
    i = s.find(p)
    if i < 0 or i < 8:
        return None
    i += 8
    bits = []
    while i + 1 < len(hb):
        a, c = hb[i], hb[i + 1]
        if a == c:
            break
        bits.append(a if bitmap == 0 else 1 - a)
        i += 2
    nbytes = len(bits) // 8
    return bytes(int(''.join(map(str, bits[k*8:k*8+8])), 2) for k in range(nbytes)), len(bits)

NATIVE = {0b1000: 'NW', 0b1001: 'NR'}
def describe_req(by):
    if not by:
        return None
    cmd = by[0] >> 4
    if len(by) < 3:
        return None
    addr = ((by[0] & 0xF) << 16) | (by[1] << 8) | by[2]
    if cmd & 0x8:
        kind = 'Native ' + ('WRITE' if (cmd & 3) == 0 else 'READ' if (cmd & 3) == 1 else f'cmd{cmd:04b}')
        ln = by[3] + 1 if len(by) > 3 else None
        data = by[4:] if (cmd & 3) == 0 else b''
        return f"{kind:12s} 0x{addr:05X} len={ln} " + (('data=' + data.hex(' ')) if data else '')
    mot = 'MOT ' if cmd & 4 else ''
    op = {0: 'I2C WRITE', 1: 'I2C READ', 2: 'I2C WRSTAT'}.get(cmd & 3, '?')
    if len(by) == 3:
        return f"{op:12s} @0x{addr & 0x7F:02X} {mot}(addr-only)"
    ln = by[3] + 1
    data = by[4:] if (cmd & 3) == 0 else b''
    return f"{op:12s} @0x{addr & 0x7F:02X} {mot}len={ln} " + (('data=' + data.hex(' ')) if data else '')

def describe_rep(by):
    if not by:
        return '(empty)'
    n = by[0] >> 4
    na = {0: 'ACK', 1: 'NACK', 2: 'DEFER'}.get(n & 3, '?')
    i2 = {0: 'ACK', 1: 'NACK', 2: 'DEFER'}.get((n >> 2) & 3, '?')
    s = na if (n >> 2) == 0 else f"{na}/I2C_{i2}"
    return f"REPLY {s:10s} " + (('data=' + by[1:].hex(' ')) if len(by) > 1 else '')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file'); ap.add_argument('-c', '--channel', type=int, default=0)
    ap.add_argument('--raw', action='store_true')
    a = ap.parse_args()
    b, idx, sr = load(a.file, a.channel)
    fr = [(t, halfbits(r)) for t, r in frames(b, idx, sr)]
    # choose polarity/bitmap by maximizing plausible request commands
    best = None
    for pol in (1, 0):
        for bm in (0, 1):
            score = 0
            for t, hb in fr[:400]:
                r = decode_frame(hb, pol, bm)
                if r and r[0]:
                    c = r[0][0] >> 4
                    if c in (0b1000, 0b1001, 0, 1, 4, 5) and r[1] % 8 == 0:
                        score += 1
            if best is None or score > best[0]:
                best = (score, pol, bm)
    _, pol, bm = best
    print(f"# frames={len(fr)} sync_pol={pol} bitmap={bm}")
    expect_req = True; last_t = -1e9
    for t, hb in fr:
        r = decode_frame(hb, pol, bm)
        if not r:
            print(f"{t/1e6:11.6f}s  ?? undecodable ({len(hb)} halfbits)"); continue
        by, nb = r
        # a reply follows a request within ~400us; otherwise this is a new request
        expect_req = True  # only source requests are decoded reliably
        if expect_req:
            d = describe_req(by)
            print(f"{t/1e6:11.6f}s  REQ  {d or by.hex(' ')}" + ('' if nb % 8 == 0 else f'  [{nb} bits]') + (f'  raw={by.hex(" ")}' if a.raw else ''))
        else:
            print(f"{t/1e6:11.6f}s       {describe_rep(by)}" + ('' if nb % 8 == 0 else f'  [{nb} bits]'))
        expect_req = not expect_req; last_t = t

main()
