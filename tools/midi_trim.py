import struct, sys
from collections import Counter

def varlen(v):
    out = bytearray()
    out.append(v & 0x7F)
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))

def read_varlen(body, p):
    ln = 0
    while True:
        b = body[p]; p += 1
        ln = (ln << 7) | (b & 0x7F)
        if not b & 0x80: break
    return ln, p

def parse(path):
    data = open(path, 'rb').read()
    pos = 0
    assert data[0:4] == b'MThd'
    hdr_len = struct.unpack('>I', data[4:8])[0]
    fmt, ntrks, div = struct.unpack('>HHH', data[8:14])
    pos = 8 + hdr_len
    tracks = []  # list of [(abs_tick, raw_bytes)]
    for t in range(ntrks):
        assert data[pos:pos+4] == b'MTrk', f"track {t}: {data[pos:pos+4]}"
        tlen = struct.unpack('>I', data[pos+4:pos+8])[0]
        body = data[pos+8:pos+8+tlen]
        pos += 8 + tlen
        p, tick, running = 0, 0, None
        events = []
        while p < len(body):
            delta = 0
            while True:
                b = body[p]; p += 1
                delta = (delta << 7) | (b & 0x7F)
                if not b & 0x80: break
            tick += delta
            st = body[p]
            if st < 0x80:
                st = running
            else:
                p += 1
                running = st if st < 0xF0 else None
            if st == 0xFF:
                mt = body[p]; p += 1
                ln, p = read_varlen(body, p)
                payload = body[p:p+ln]; p += ln
                if mt != 0x2F:
                    events.append((tick, bytes([0xFF, mt]) + varlen(ln) + payload))
            elif st in (0xF0, 0xF7):
                ln, p = read_varlen(body, p)
                events.append((tick, bytes([st]) + varlen(ln) + body[p:p+ln]))
                p += ln
            else:
                nb = 2 if (st & 0xF0) not in (0xC0, 0xD0) else 1
                events.append((tick, bytes([st]) + body[p:p+nb]))
                p += nb
        tracks.append(events)
    return fmt, ntrks, div, tracks

def is_note_on(raw):
    return (raw[0] & 0xF0) == 0x90 and len(raw) >= 3 and raw[2] > 0

def scan(tracks, div):
    for ti, evs in enumerate(tracks):
        c = Counter(raw[0] & 0xF0 if raw[0] < 0xF0 else raw[0] for _, raw in evs)
        print(f"track {ti}: events={len(evs)} types={dict(c)}")
    first_note = last_note = None
    note_count = 0
    for evs in tracks:
        for tick, raw in evs:
            if is_note_on(raw):
                note_count += 1
                first_note = tick if first_note is None else min(first_note, tick)
                last_note = tick if last_note is None else max(last_note, tick)
    end_tick = max((t for evs in tracks for t, _ in evs), default=0)
    sec = lambda t: t * 500000 / 1e6 / div / 60
    print(f"total notes      : {note_count}")
    if first_note is not None:
        print(f"first note event : tick {first_note} = {sec(first_note):.3f} min")
        print(f"last note event  : tick {last_note} = {sec(last_note):.3f} min")
        print(f"silent head      : {sec(first_note):.3f} min")
    print(f"file end         : tick {end_tick} = {sec(end_tick):.3f} min")

def trim(src, dst, lead_in_ticks):
    fmt, ntrks, div, tracks = parse(src)
    first_note = min((t for evs in tracks for t, raw in evs if is_note_on(raw)),
                     default=None)
    assert first_note is not None, "no notes found"
    shift = max(0, first_note - lead_in_ticks)
    print(f"first note at tick {first_note}; shifting by {shift} ticks")
    out = bytearray(b'MThd' + struct.pack('>IHHH', 6, fmt, len(tracks), div))
    total_notes = 0
    for evs in tracks:
        body = bytearray()
        prev = 0
        for tick, raw in sorted(evs, key=lambda e: e[0]):
            nt = tick - shift
            if nt < 0:
                continue
            if is_note_on(raw):
                total_notes += 1
            body += varlen(nt - prev) + raw
            prev = nt
        body += varlen(0) + b'\xFF\x2F\x00'
        out += b'MTrk' + struct.pack('>I', len(body)) + body
    open(dst, 'wb').write(out)
    print(f"wrote {dst} ({len(out)} bytes), {total_notes} note-ons")

if __name__ == '__main__':
    src = sys.argv[1]
    if src == '--scan':
        _, ntrks, div, tracks = parse(sys.argv[2])
        scan(tracks, div)
    else:
        dst = sys.argv[2]
        lead = int(sys.argv[3]) if len(sys.argv) > 3 else 960
        trim(src, dst, lead)