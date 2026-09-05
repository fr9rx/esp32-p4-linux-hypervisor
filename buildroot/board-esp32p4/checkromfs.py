#!/usr/bin/env python3
"""Check a romfs image by re-implementing the kernel's reader, then compare the
result against the directory it was built from. Mirrors fs/romfs/super.c.

Note the name of an entry comes from the entry itself (super.c:186), while the
inode it refers to comes from following spec when the type is a hard link
(super.c:198). Reading the name from the resolved target instead makes every
directory's "." look like a real child and recurses forever."""
import os, stat, struct, sys

HRD, DIR, REG, SYM = 0, 1, 2, 3
ALIGN = 16
img = open(sys.argv[1], "rb").read()
src = sys.argv[2]

assert img[0:8] == b"-rom1fs-", "bad magic"
size = struct.unpack(">I", img[8:12])[0]
assert size == len(img), "size %d != file %d" % (size, len(img))
span = min(size, 512)
words = struct.unpack(">%dI" % (span // 4), img[:span])
assert sum(words) & 0xFFFFFFFF == 0, "superblock checksum != 0"

nlen = img.index(b"\0", 16) - 16
root = (ALIGN + nlen + 1 + 15) & ~15


def hdr(pos):
    nxt, spec, sz, _ = struct.unpack(">IIII", img[pos:pos + 16])
    name = img[pos + 16:img.index(b"\0", pos + 16)].decode()
    return nxt, spec, sz, name


def resolve(pos):
    while True:
        nxt, spec, sz, name = hdr(pos)
        if (nxt & 7) != HRD:
            return pos, nxt, spec, sz
        pos = spec & ~15


def data(pos, sz):
    name = hdr(pos)[3]
    meta = (ALIGN + len(name) + 1 + 15) & ~15
    return img[pos + meta:pos + meta + sz]


found = {}


def walk(pos, path, depth=0):
    assert depth < 40, "runaway at " + path
    off = resolve(pos)[2] & ~15
    while off and off < size:
        entry_nxt, _, _, name = hdr(off)          # name: from the entry
        tpos, tnxt, _, tsz = resolve(off)         # inode: after following HRDs
        kind = tnxt & 7
        if name not in (".", ".."):
            full = path + "/" + name
            if kind == DIR:
                found[full] = ("dir", None)
                walk(tpos, full, depth + 1)
            elif kind == SYM:
                found[full] = ("sym", data(tpos, tsz))
            elif kind == REG:
                found[full] = ("reg", data(tpos, tsz))
            else:
                found[full] = ("other", None)
        off = entry_nxt & ~15


walk(root, "")

expect = {}
for dirpath, dirnames, filenames in os.walk(src):
    rel = "" if dirpath == src else "/" + os.path.relpath(dirpath, src)
    for n in dirnames + filenames:
        p = os.path.join(dirpath, n)
        st = os.lstat(p)
        if stat.S_ISLNK(st.st_mode):
            expect[rel + "/" + n] = ("sym", os.readlink(p).encode())
        elif stat.S_ISDIR(st.st_mode):
            expect[rel + "/" + n] = ("dir", None)
        elif stat.S_ISREG(st.st_mode):
            expect[rel + "/" + n] = ("reg", open(p, "rb").read())
        else:
            expect[rel + "/" + n] = ("other", None)

missing = sorted(set(expect) - set(found))
extra = sorted(set(found) - set(expect))
bad = [k for k in sorted(set(expect) & set(found)) if expect[k] != found[k]]

print("image %d bytes, root inode at 0x%x" % (size, root))
print("entries: expected %d, read back %d" % (len(expect), len(found)))
print("missing:", missing[:10] or "none")
print("extra:  ", extra[:10] or "none")
print("mismatched:", [(k, expect[k][0], found[k][0]) for k in bad[:10]] or "none")
for probe in ("/bin/busybox", "/bin/sh", "/etc/inittab", "/sbin/init", "/sbin/getty"):
    e = found.get(probe)
    print("  %-16s %s %s" % (probe, e[0] if e else "ABSENT",
                             len(e[1]) if e and e[1] else ""))
print("VERDICT:", "OK" if not (missing or extra or bad) else "MISMATCH")
