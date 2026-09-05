#!/usr/bin/env python3
"""Build a romfs image from a directory tree.

Buildroot 2026.05 removed its romfs support (Config.in.legacy) and there is no
genromfs available here, so this writes the image directly. The format is taken
from the kernel we boot: include/uapi/linux/romfs_fs.h and fs/romfs/super.c.

  superblock: "-rom1fs-", be32 size, be32 checksum, NUL-terminated volume name,
              padded to 16 bytes. The root inode lives at the first 16-byte
              boundary after it -- super.c:531 computes exactly that.
  inode:      be32 next (offset of the next sibling, with the type in the low 3
              bits and the exec bit in bit 3), be32 spec, be32 size,
              be32 checksum, NUL-terminated name padded to 16, then the data.
  directory:  spec = offset of the first child. readdir follows spec, then the
              next-chain until next is 0 (super.c:169,204).
  hard link:  spec = offset of the target inode; iget follows the chain
              (super.c:290-303). "." and ".." are emitted this way, which is
              what genromfs does and what the kernel expects.

romfs keeps no per-file permissions -- the kernel maps type to a fixed mode and
only honours the exec bit (super.c:329-344), so ownership and setuid do not
survive. Everything here runs as root, so that costs nothing.

Only the superblock checksum is verified, and only over the first 512 bytes
(super.c:515), but per-inode checksums are filled in anyway.
"""
import os, stat, struct, sys

HRD, DIR, REG, SYM, BLK, CHR, SCK, FIF = range(8)
EXEC = 8
ALIGN = 16


PAGE = 4096

def pad16(n):
    return (n + 15) & ~15


class Node:
    def __init__(self, name, kind, data=b"", spec_dev=0):
        self.name = name
        self.kind = kind
        self.data = data
        self.spec_dev = spec_dev
        self.children = []
        self.off = 0
        self.spec = 0
        self.nxt = 0

    @property
    def hdr_len(self):
        return pad16(ALIGN + len(self.name) + 1)

    @property
    def total_len(self):
        return self.hdr_len + pad16(len(self.data))
    # Executables get their data placed on a page boundary. binfmt_flat's
    # XIP path mmaps a binary's text straight from the file, and on romfs
    # that resolves to a pointer into the mtd-rom window. NOMMU does not
    # insist the result be page aligned, but every other consumer of a
    # direct mapping does, and one page of padding for one file is cheaper
    # than finding out which.
    @property
    def wants_page_align(self):
        return ((self.kind & 7) == REG and (self.kind & EXEC)
                and len(self.data) >= PAGE)


def scan(path, name):
    st = os.lstat(path)
    if stat.S_ISDIR(st.st_mode):
        node = Node(name, DIR | EXEC)
        for entry in sorted(os.listdir(path)):
            child = scan(os.path.join(path, entry), entry)
            if child is not None:
                node.children.append(child)
        return node
    if stat.S_ISLNK(st.st_mode):
        return Node(name, SYM, os.readlink(path).encode())
    if stat.S_ISREG(st.st_mode):
        kind = REG | (EXEC if st.st_mode & 0o111 else 0)
        with open(path, "rb") as f:
            return Node(name, kind, f.read())
    if stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
        kind = CHR if stat.S_ISCHR(st.st_mode) else BLK
        dev = (os.major(st.st_rdev) << 16) | (os.minor(st.st_rdev) & 0xFFFF)
        return Node(name, kind, b"", dev)
    if stat.S_ISFIFO(st.st_mode):
        return Node(name, FIF)
    if stat.S_ISSOCK(st.st_mode):
        return Node(name, SCK)
    return None


def layout(root, volume):
    """Assign every node an offset. Returns (flat node list, image size)."""
    cursor = pad16(ALIGN + len(volume) + 1)
    root.off = cursor
    cursor += root.hdr_len
    flat = [root]

    # A directory's entries are laid out contiguously, so a sibling's `next` is
    # simply the offset that follows it. Subdirectories are queued rather than
    # recursed into, which keeps each run of entries unbroken.
    queue = [(root, root)]
    while queue:
        node, parent = queue.pop(0)
        dot, dotdot = Node(".", HRD), Node("..", HRD)
        dot.spec, dotdot.spec = node.off, parent.off
        entries = [dot, dotdot] + node.children

        node.spec = cursor
        for entry in entries:
            if entry.wants_page_align:
                # Pad ahead of the header so the DATA lands on the
                # boundary. Gaps are free: sibling links are explicit
                # offsets, not implied adjacency.
                overshoot = (cursor + entry.hdr_len) % PAGE
                if overshoot:
                    cursor += PAGE - overshoot
            entry.off = cursor
            cursor += entry.total_len
        for i, entry in enumerate(entries):
            entry.nxt = entries[i + 1].off if i + 1 < len(entries) else 0

        flat += entries
        for entry in node.children:
            if (entry.kind & 7) == DIR:
                queue.append((entry, node))
    return flat, cursor


def emit(node, buf):
    spec = node.spec_dev if (node.kind & 7) in (CHR, BLK) else node.spec
    name = node.name.encode() + b"\0"
    name += b"\0" * (node.hdr_len - ALIGN - len(name))
    block = bytearray(struct.pack(">IIII", node.nxt | node.kind, spec,
                                  len(node.data), 0) + name)

    # Make the header, name included, sum to zero as big-endian 32-bit words --
    # the same convention the superblock uses.
    words = struct.unpack(">%dI" % (len(block) // 4), bytes(block))
    struct.pack_into(">I", block, 12, (-sum(words)) & 0xFFFFFFFF)
    buf[node.off:node.off + len(block)] = block

    off = node.off + node.hdr_len
    padded = node.data + b"\0" * (pad16(len(node.data)) - len(node.data))
    buf[off:off + len(padded)] = padded


def main():
    src, out = sys.argv[1], sys.argv[2]
    volume = sys.argv[3] if len(sys.argv) > 3 else "rootfs"

    flat, size = layout(scan(src, "."), volume)
    buf = bytearray(size)

    buf[0:8] = b"-rom1fs-"
    struct.pack_into(">I", buf, 8, size)
    volname = volume.encode() + b"\0"
    buf[16:16 + len(volname)] = volname

    for node in flat:
        emit(node, buf)

    span = min(size, 512)
    words = struct.unpack(">%dI" % (span // 4), bytes(buf[:span]))
    struct.pack_into(">I", buf, 12, (-sum(words)) & 0xFFFFFFFF)

    with open(out, "wb") as f:
        f.write(buf)
    print("%s: %d bytes, %d inodes, volume %r" % (out, size, len(flat), volume))


if __name__ == "__main__":
    main()
