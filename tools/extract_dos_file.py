"""Extract a file from the MS-DOS 2.11 floppy image (FAT12) for inspection."""
import struct, sys

img = sys.argv[1]
_raw = sys.argv[2]
if '.' in _raw:
    _b, _e = _raw.rsplit('.', 1)
else:
    _b, _e = _raw, ''
name = (_b.upper()[:8].ljust(8) + _e.upper()[:3].ljust(3)).encode()
off = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
ln = int(sys.argv[4], 0) if len(sys.argv) > 4 else 64

data = open(img, 'rb').read()
bs = data[:512]
spc = bs[13]
resv = struct.unpack('<H', bs[14:16])[0]
nfats = bs[16]
rootent = struct.unpack('<H', bs[17:19])[0]
fatsec = struct.unpack('<H', bs[22:24])[0]
root_lba = resv + nfats * fatsec
root_secs = (rootent * 32 + 511) // 512
data_lba = root_lba + root_secs

def fat12(cl):
    b = fatsec and (fatsec > 0)
    # FAT starts at resv
    fat = data[resv * 512: resv * 512 + fatsec * 512]
    o = cl * 3 // 2
    w = fat[o] | (fat[o + 1] << 8)
    return (w >> 4) if (cl & 1) else (w & 0xFFF)

# find entry
ent = None
for s in range(root_secs):
    base = root_lba * 512 + s * 512
    blk = data[base:base + 512]
    for i in range(0, 512, 32):
        e = blk[i:i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5:
            continue
        if e[11] & 0x3F == 0x0F:
            continue
        if e[:11] == name:
            ent = e
            break
    if ent:
        break
if not ent:
    raise SystemExit('not found')
cl = struct.unpack('<H', ent[26:28])[0]
size = struct.unpack('<I', ent[28:32])[0]
chain = []
c = cl
while 2 <= c < 0xFF8:
    chain.append(c)
    c = fat12(c)
print('cluster', cl, 'size', size, 'chain', chain[:10], '...' if len(chain) > 10 else '')
# read whole file
buf = bytearray()
for c in chain:
    base = (data_lba + (c - 2) * spc) * 512
    buf += data[base:base + spc * 512]
buf = buf[:size]
print('file size', len(buf))
for i in range(0, min(ln, len(buf)), 16):
    chunk = buf[off + i: off + i + 16]
    if not chunk:
        break
    print('%04X: %s  %s' % (off + i, ' '.join('%02X' % b for b in chunk),
                            ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)))
