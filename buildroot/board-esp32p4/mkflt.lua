#!/usr/bin/env lua
--[[----------------------------------------------------------------------
mkflt.lua -- turn an rv32 object file into a runnable bFLT binary, on the
board, with objcopy for the bytes and no linker at all.

    as prog.S -o prog.o
    lua /usr/lib/mkflt.lua prog.o prog
    ./prog

(mkflt(1) runs both steps for you.)

WHY THIS EXISTS

ld is 5,283,840 bytes and cannot run here. binfmt_flat makes ONE contiguous
allocation per exec, and this guest's allocator has no block that big once it
has booted:

    nommu: Allocation of length 5283840 from process 36 (ld) failed
    Normal: ... 3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25400kB

25 MB free and not one block big enough, every time. elf2flt consumes ld's
output, so it went too. What is left on the board is as (1.3 MB), objcopy
(956 kB) and lua (300 kB) -- and between them that is enough, because a
single-object freestanding program needs only two things a linker would do:

  1. lay the allocatable sections out end to end and give symbols addresses,
  2. apply the relocations the assembler left behind.

Both are below. It is a linker in the sense that matters here and in no other
sense: one object file, no archives, no symbol resolution across files, no GOT.

WHY NOT JUST WRITE RELOCATION-FREE ASSEMBLY

That was the first attempt and it does not work. rv32 gas will not fold a
label difference into an instruction immediate at ALL -- not `addi a1, a1,
msg - 1b`, not through .equ, not with %lo(), not with relaxation disabled:

    Error: illegal operands `addi a1,a1,msg-1b'

Only literal constants are accepted. So every address-of in hand-written
assembly leaves a relocation, and something has to apply it. Hence this file,
and hence `lla` and `call` work here exactly as they normally would.

WHAT IS NOT SUPPORTED

  * R_RISCV_GOT_HI20 -- `la` under `.option pic`. There is no GOT in a flat
    binary built this way. Use `lla`.
  * Undefined symbols. One object file, no libraries: if it is not in this
    file, nothing can resolve it.
  * R_RISCV_ALIGN / R_RISCV_RELAX -- these exist so a LINKER can delete
    instructions, and deleting instructions would move every symbol. mkflt
    assembles with -mno-relax so they never appear; if you run `as` yourself,
    do the same.

Each of those is reported by name rather than by producing a binary that
jumps to zero.

ONE SEGMENT, NO LOAD-TIME RELOCATIONS

This is the part that took a wrong turn first, so it is worth stating plainly.

bFLT has three regions -- text, data, bss -- and they are NOT laid out
contiguously in memory. fs/binfmt_flat.c:

    realdatastart = textpos + ntohl(hdr->data_start);
    datapos = ALIGN(realdatastart + DATA_START_OFFSET_WORDS * 4,
                    FLAT_DATA_ALIGN);          /* FLAT_DATA_ALIGN is 0x20 */

Data starts at a 32-byte boundary PAST the end of text, and bss follows data.
So `lla a1, msg` -- which computes an address as PC plus a distance fixed at
link time -- is only correct if the thing it points at is in the same region as
the instruction. That gap is the whole reason elf2flt-built binaries reach
their data through a global offset table and set FLAT_FLAG_GOTPIC. Hand-written
assembly has no GOT.

The resolution here is to not use the other two regions at all:

  * code, rodata, data and bss all go in the text region, contiguous, exactly
    as the relocations assume. bss is materialised as zeroes in the file rather
    than declared -- it costs file size and buys correctness.
  * data and bss are declared EMPTY: data_start = data_end = bss_end = the end
    of the image.
  * there is no relocation table. reloc_count is 0.

Trying to keep a relocation table with an empty data segment is what the first
version did, and the loader read it from the wrong place:

    binfmt_flat: reloc outside program 0x84000000 (0 - 0xd0/0x8e)

0x84000000 is ntohl() of the little-endian word 0x00000084 -- the program's own
data pointer. The table is addressed relative to datapos, and datapos is not
where a single blob lives.

What this costs you: `.word some_symbol` in data cannot work. An absolute
address has to be fixed up at load time, and load-time fixups are the thing
that has just been given up. mkflt.lua refuses those by name. Compute the
address at runtime with `lla` instead -- it is one instruction pair and it is
position-independent, which the stored pointer never was.

THE HEADER

include/uapi/linux/flat.h. All 32-bit fields BIG-ENDIAN on disk regardless of
the machine; fs/binfmt_flat.c byte-swaps on read.

    0   char magic[4]     "bFLT"
    4   u32  rev          4
    8   u32  entry        offset of the entry point from the START of the file
    12  u32  data_start
    16  u32  data_end
    20  u32  bss_end
    24  u32  stack_size
    28  u32  reloc_start
    32  u32  reloc_count
    36  u32  flags
    40  u32  build_date
    44  u32  filler[5]

Header is 64 bytes and every offset is from the start of the file.
------------------------------------------------------------------------]]

local FLAT_MAGIC    = "bFLT"
local FLAT_VERSION  = 4
local FLAT_HDR_SIZE = 64
local FLAT_FLAG_RAM = 0x1
local DEFAULT_STACK = 8192

-- ELF32 constants
local SHT_PROGBITS, SHT_SYMTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 1, 2, 4, 8, 9
local SHF_ALLOC = 0x2
local SHN_UNDEF, SHN_ABS = 0, 0xfff1
local EM_RISCV, ET_REL = 243, 1

-- RISC-V relocation types we handle. Names are for error messages; a
-- relocation this table does not list is a hard error, never a silent zero.
local R = {
    NONE = 0, RV32 = 1, BRANCH = 16, JAL = 17, CALL = 18, CALL_PLT = 19,
    GOT_HI20 = 20, PCREL_HI20 = 23, PCREL_LO12_I = 24, PCREL_LO12_S = 25,
    HI20 = 26, LO12_I = 27, LO12_S = 28,
    ADD8 = 33, ADD16 = 34, ADD32 = 35, SUB8 = 37, SUB16 = 38, SUB32 = 39,
    ALIGN = 43, RVC_BRANCH = 44, RVC_JUMP = 45, RELAX = 51,
    SUB6 = 52, SET6 = 53, SET8 = 54, SET16 = 55, SET32 = 56, RV32_PCREL = 57,
}
local RNAME = {}
for k, v in pairs(R) do RNAME[v] = "R_RISCV_" .. k end

local function die(fmt, ...)
    io.stderr:write("mkflt.lua: " .. string.format(fmt, ...) .. "\n")
    os.exit(1)
end

------------------------------------------------------------------------
-- byte access
--
-- This lua is built with LUA_32BITS, so integers are 32-bit and the number
-- type is a 32-bit float with a 24-bit mantissa. Everything here therefore
-- stays in integer arithmetic (// and bitwise ops, never /), and no value
-- larger than a file offset is ever built.
------------------------------------------------------------------------
local d      -- the object file, as a string

local function u8(o)  return d:byte(o + 1) end
local function u16(o) return u8(o) | (u8(o + 1) << 8) end
local function u32(o)
    return u8(o) | (u8(o+1) << 8) | (u8(o+2) << 16) | (u8(o+3) << 24)
end
-- Sign-extend explicitly rather than relying on the integer width. On the
-- board this lua has 32-bit integers and u32 already wraps to negative, but
-- the same script has to give the same answer under a 64-bit host lua when it
-- is being tested -- and a relocation addend is routinely negative.
local function s32(o)
    local v = u32(o)
    if v >= 0x80000000 then v = v - 0x100000000 end
    return v
end

local function be32(n)
    return string.char((n >> 24) & 0xff, (n >> 16) & 0xff,
                       (n >> 8) & 0xff, n & 0xff)
end

------------------------------------------------------------------------
-- arguments
------------------------------------------------------------------------
local obj, out, stack, objcopy = nil, nil, DEFAULT_STACK, "objcopy"
local i = 1
while arg[i] do
    local a = arg[i]
    if a == "-s" then
        i = i + 1
        stack = tonumber(arg[i]) or die("-s wants a number")
    elseif a == "--objcopy" then
        i = i + 1
        objcopy = arg[i] or die("--objcopy wants a path")
    elseif a:sub(1, 1) == "-" then
        die("unknown option %s", a)
    elseif not obj then obj = a
    elseif not out then out = a
    else die("too many arguments") end
    i = i + 1
end
if not obj or not out then
    io.stderr:write("usage: mkflt.lua OBJECT.o OUTPUT [-s stack] " ..
                    "[--objcopy PATH]\n")
    os.exit(2)
end
if stack < 1024 then die("stack size %d is below the 1024-byte floor", stack) end

------------------------------------------------------------------------
-- read and validate the object
------------------------------------------------------------------------
local fh = io.open(obj, "rb") or die("cannot read %s", obj)
d = fh:read("a")
fh:close()

if #d < 52 or d:sub(1, 4) ~= "\127ELF" then
    die("%s is not an ELF file", obj)
end
if u8(4) ~= 1 then die("%s is not ELF32", obj) end
if u8(5) ~= 1 then die("%s is not little-endian", obj) end
if u16(16) ~= ET_REL then
    die("%s is not a relocatable object (as leaves ET_REL; this is type %d)",
        obj, u16(16))
end
if u16(18) ~= EM_RISCV then die("%s is not RISC-V", obj) end

local e_shoff, e_shentsize = u32(32), u16(46)
local e_shnum, e_shstrndx  = u16(48), u16(50)
if e_shoff == 0 or e_shnum == 0 then die("%s has no section headers", obj) end

------------------------------------------------------------------------
-- sections
------------------------------------------------------------------------
local shstr = u32(e_shoff + e_shstrndx * e_shentsize + 16)
local function shname(off)
    return d:sub(shstr + off + 1):match("^[^%z]*")
end

local sec = {}
for n = 0, e_shnum - 1 do
    local o = e_shoff + n * e_shentsize
    sec[n] = {
        index  = n,
        name   = shname(u32(o)),
        type   = u32(o + 4),
        flags  = u32(o + 8),
        offset = u32(o + 16),
        size   = u32(o + 20),
        link   = u32(o + 24),
        info   = u32(o + 28),
        align  = u32(o + 32),
        entsize= u32(o + 36),
    }
end

------------------------------------------------------------------------
-- layout
--
-- Allocatable sections end to end, each on its own alignment, with the ones
-- that have contents first and .bss-style NOBITS last. A flat binary has no
-- section table, so this ordering IS the memory map.
------------------------------------------------------------------------
local base = {}          -- section index -> offset within the loaded image
local image_len = 0

local function place(want_nobits)
    for n = 0, e_shnum - 1 do
        local s = sec[n]
        local is_nobits = (s.type == SHT_NOBITS)
        if (s.flags & SHF_ALLOC) ~= 0 and s.size > 0
           and is_nobits == want_nobits then
            local a = s.align
            if a > 1 then
                image_len = ((image_len + a - 1) // a) * a
            end
            base[n] = image_len
            image_len = image_len + s.size
        end
    end
end

place(false)                 -- .text, .rodata, .data
local stored_end = image_len -- what came out of the object file
place(true)                  -- .bss, materialised as zeroes rather than
                             -- declared: see the header comment
local bss_len = image_len - stored_end

if stored_end == 0 then die("%s has no allocatable content", obj) end

------------------------------------------------------------------------
-- section bytes, via objcopy
--
-- objcopy rather than reading sh_offset directly: it is the tool for this,
-- it is on the board, and --only-section keeps each section's bytes separate
-- (a plain `objcopy -O binary` of an unlinked object lays every section at
-- VMA 0 and they land on top of each other).
------------------------------------------------------------------------
local tmpdir = os.getenv("TMPDIR") or "/tmp"
local tmp = string.format("%s/mkflt.%d.bin", tmpdir, os.time() % 100000)

local image = {}             -- array of bytes, 1-based, image offset + 1
for n = 0, image_len - 1 do image[n + 1] = 0 end

local function blit(at, bytes)
    for k = 1, #bytes do image[at + k] = bytes:byte(k) end
end

for n = 0, e_shnum - 1 do
    local s = sec[n]
    if base[n] and s.type ~= SHT_NOBITS then
        local cmd = string.format("%s -O binary --only-section=%s %s %s",
                                  objcopy, s.name, obj, tmp)
        local ok = os.execute(cmd)
        if not ok then die("objcopy failed on section %s", s.name) end
        local f = io.open(tmp, "rb") or die("objcopy wrote nothing for %s", s.name)
        local bytes = f:read("a")
        f:close()
        if #bytes ~= s.size then
            die("objcopy gave %d bytes for %s, section header says %d",
                #bytes, s.name, s.size)
        end
        blit(base[n], bytes)
    end
end
os.remove(tmp)

local function rd32(off)
    return image[off+1] | (image[off+2] << 8)
         | (image[off+3] << 16) | (image[off+4] << 24)
end
local function wr32(off, v)
    image[off+1] = v & 0xff
    image[off+2] = (v >> 8) & 0xff
    image[off+3] = (v >> 16) & 0xff
    image[off+4] = (v >> 24) & 0xff
end

------------------------------------------------------------------------
-- symbols
------------------------------------------------------------------------
local symtab, strtab
for n = 0, e_shnum - 1 do
    if sec[n].type == SHT_SYMTAB then
        symtab = sec[n]
        strtab = sec[sec[n].link]
    end
end
if not symtab then die("%s has no symbol table", obj) end

local function symname(off)
    return d:sub(strtab.offset + off + 1):match("^[^%z]*")
end

local nsyms = symtab.size // 16
local sym = {}
for k = 0, nsyms - 1 do
    local o = symtab.offset + k * 16
    local shndx = u16(o + 14)
    local value = u32(o + 4)
    local addr
    if shndx == SHN_ABS then
        addr = value
    elseif shndx == SHN_UNDEF then
        addr = nil                      -- reported at use, with its name
    elseif base[shndx] then
        addr = base[shndx] + value
    else
        addr = value                    -- non-allocatable: debug info etc.
    end
    sym[k] = { name = symname(u32(o)), addr = addr, shndx = shndx }
end

local function symaddr(k, rtype)
    local s = sym[k] or die("relocation names symbol %d, which does not exist", k)
    if not s.addr then
        die("undefined symbol '%s'.\n" ..
            "mkflt.lua: there is no linker and no libraries here -- every " ..
            "symbol must be\nmkflt.lua: defined in %s itself.",
            s.name ~= "" and s.name or ("#" .. k), obj)
    end
    return s.addr
end

------------------------------------------------------------------------
-- relocations
------------------------------------------------------------------------
local hi20_at = {}           -- image offset of a HI20 site -> its computed value

-- Instruction field encoders. Each takes the existing word and the value and
-- returns the patched word.
local function put_u(w, v)   -- auipc/lui: imm in 31:12
    return (w & 0xfff) | ((v & 0xfffff) << 12)
end
local function put_i(w, v)   -- addi/jalr/lw: imm in 31:20
    return (w & 0xfffff) | ((v & 0xfff) << 20)
end
local function put_s(w, v)   -- sw: imm[11:5] in 31:25, imm[4:0] in 11:7
    return (w & 0x1fff07f)
         | (((v >> 5) & 0x7f) << 25)
         | ((v & 0x1f) << 7)
end
local function put_b(w, v)   -- branches
    return (w & 0x1fff07f)
         | (((v >> 12) & 0x1) << 31)
         | (((v >> 5) & 0x3f) << 25)
         | (((v >> 1) & 0xf) << 8)
         | (((v >> 11) & 0x1) << 7)
end
local function put_j(w, v)   -- jal
    return (w & 0xfff)
         | (((v >> 20) & 0x1) << 31)
         | (((v >> 1) & 0x3ff) << 21)
         | (((v >> 11) & 0x1) << 20)
         | (((v >> 12) & 0xff) << 12)
end

-- The +0x800 is the standard auipc/addi pairing: the low 12 bits are added
-- back as a SIGNED value, so the high part has to round rather than truncate.
local function hi20(v) return (v + 0x800) >> 12 end

local function check_range(v, bits, what, off)
    local limit = 1 << (bits - 1)
    if v >= limit or v < -limit then
        die("%s at image offset 0x%x is %d bytes away, out of %d-bit range.\n" ..
            "mkflt.lua: a linker would relax this; there is no linker.",
            what, off, v, bits)
    end
end

local nrelocs = 0
for n = 0, e_shnum - 1 do
    local s = sec[n]
    if s.type == SHT_RELA or s.type == SHT_REL then
        local target = s.info
        if base[target] then
            local esz = (s.type == SHT_RELA) and 12 or 8
            for k = 0, s.size // esz - 1 do
                local o = s.offset + k * esz
                local r_off  = u32(o)
                local r_info = u32(o + 4)
                local addend = (s.type == SHT_RELA) and s32(o + 8) or 0
                local rtype  = r_info & 0xff
                local rsym   = (r_info >> 8) & 0xffffff
                local at     = base[target] + r_off      -- image offset
                nrelocs = nrelocs + 1

                if rtype == R.NONE or rtype == R.RELAX then
                    -- nothing to do; RELAX is a hint for a linker we do not have

                elseif rtype == R.RV32 then
                    -- An absolute address stored in data -- `.word symbol`.
                    -- It would have to be fixed up at load time, and this
                    -- format's load-time fixups live in a data segment that is
                    -- not contiguous with the single segment everything else
                    -- is in. See the header comment.
                    die("R_RISCV_32 at image offset 0x%x: a stored pointer " ..
                        "(.word %s).\nmkflt.lua: that needs a load-time " ..
                        "relocation, which a single-segment flat binary " ..
                        "cannot have.\nmkflt.lua: compute the address at run " ..
                        "time instead:  lla reg, %s",
                        at, sym[rsym] and sym[rsym].name or "?",
                        sym[rsym] and sym[rsym].name or "sym")

                elseif rtype == R.RV32_PCREL then
                    wr32(at, symaddr(rsym) + addend - at)

                elseif rtype == R.PCREL_HI20 or rtype == R.GOT_HI20 then
                    if rtype == R.GOT_HI20 then
                        die("R_RISCV_GOT_HI20 at image offset 0x%x.\n" ..
                            "mkflt.lua: `la` under .option pic needs a global " ..
                            "offset table, and a flat\nmkflt.lua: binary built " ..
                            "this way has none. Use `lla`.", at)
                    end
                    local v = symaddr(rsym) + addend - at
                    hi20_at[at] = v
                    wr32(at, put_u(rd32(at), hi20(v)))

                elseif rtype == R.PCREL_LO12_I or rtype == R.PCREL_LO12_S then
                    -- The symbol of a PCREL_LO12 is the LABEL ON THE HI20
                    -- INSTRUCTION, not the thing being addressed. Its value is
                    -- whatever that HI20 computed.
                    local site = symaddr(rsym)
                    local v = hi20_at[site]
                    if not v then
                        die("R_RISCV_%s at 0x%x refers to 0x%x, which is not a " ..
                            "PCREL_HI20 site",
                            rtype == R.PCREL_LO12_I and "PCREL_LO12_I"
                                                     or "PCREL_LO12_S", at, site)
                    end
                    local lo = v - (hi20(v) << 12)
                    if rtype == R.PCREL_LO12_I then
                        wr32(at, put_i(rd32(at), lo))
                    else
                        wr32(at, put_s(rd32(at), lo))
                    end

                elseif rtype == R.HI20 then
                    local v = symaddr(rsym) + addend
                    wr32(at, put_u(rd32(at), hi20(v)))
                elseif rtype == R.LO12_I then
                    local v = symaddr(rsym) + addend
                    wr32(at, put_i(rd32(at), v - (hi20(v) << 12)))
                elseif rtype == R.LO12_S then
                    local v = symaddr(rsym) + addend
                    wr32(at, put_s(rd32(at), v - (hi20(v) << 12)))

                elseif rtype == R.BRANCH then
                    local v = symaddr(rsym) + addend - at
                    check_range(v, 13, "branch", at)
                    wr32(at, put_b(rd32(at), v))

                elseif rtype == R.JAL then
                    local v = symaddr(rsym) + addend - at
                    check_range(v, 21, "jal", at)
                    wr32(at, put_j(rd32(at), v))

                elseif rtype == R.CALL or rtype == R.CALL_PLT then
                    -- auipc + jalr pair, patched together.
                    local v = symaddr(rsym) + addend - at
                    wr32(at, put_u(rd32(at), hi20(v)))
                    wr32(at + 4, put_i(rd32(at + 4), v - (hi20(v) << 12)))

                elseif rtype == R.ADD8 or rtype == R.ADD16 or rtype == R.ADD32
                    or rtype == R.SUB8 or rtype == R.SUB16 or rtype == R.SUB32
                    or rtype == R.SUB6 or rtype == R.SET6 or rtype == R.SET8
                    or rtype == R.SET16 or rtype == R.SET32 then
                    -- These implement label arithmetic in data (.eh_frame,
                    -- .debug_*, and hand-written .word a - b). Only the 32-bit
                    -- forms can appear in something we load; the rest are in
                    -- non-allocatable sections and never reach here.
                    local v = symaddr(rsym) + addend
                    if rtype == R.ADD32 then wr32(at, rd32(at) + v)
                    elseif rtype == R.SUB32 then wr32(at, rd32(at) - v)
                    elseif rtype == R.SET32 then wr32(at, v)
                    else
                        die("R_RISCV_%s at image offset 0x%x is not supported " ..
                            "in loadable data", RNAME[rtype] or rtype, at)
                    end

                elseif rtype == R.ALIGN then
                    die("R_RISCV_ALIGN at image offset 0x%x.\n" ..
                        "mkflt.lua: this object was assembled with relaxation " ..
                        "on, which leaves the\nmkflt.lua: instruction stream for " ..
                        "a linker to shrink. Assemble with -mno-relax.", at)

                else
                    die("unsupported relocation %s (%d) at image offset 0x%x",
                        RNAME[rtype] or "type", rtype, at)
                end
            end
        end
    end
end

------------------------------------------------------------------------
-- entry point
--
-- _start if it exists, offset 0 otherwise. Offset 0 is almost always right --
-- .text comes first and the first instruction is the program -- but saying so
-- out loud beats silently entering at whatever landed there.
------------------------------------------------------------------------
local entry = nil
for k = 0, nsyms - 1 do
    if sym[k].name == "_start" and sym[k].addr then entry = sym[k].addr end
end
if not entry then
    io.stderr:write("mkflt.lua: note: no _start; entering at offset 0\n")
    entry = 0
end

------------------------------------------------------------------------
-- write it out
------------------------------------------------------------------------
-- string.char takes its bytes as arguments and there is a limit on how many a
-- call may have, so the image is assembled in chunks. 200 is comfortably under
-- any interpreter's limit and the concatenation cost is irrelevant at these
-- sizes.
local parts = {}
for k = 1, image_len, 200 do
    local last = k + 199
    if last > image_len then last = image_len end
    parts[#parts + 1] = string.char(table.unpack(image, k, last))
end
local blob = table.concat(parts)

local image_end = FLAT_HDR_SIZE + image_len

local header = FLAT_MAGIC
    .. be32(FLAT_VERSION)
    .. be32(FLAT_HDR_SIZE + entry)
    .. be32(image_end)                    -- data_start: data is empty...
    .. be32(image_end)                    -- data_end
    .. be32(image_end)                    -- bss_end: ...and so is bss
    .. be32(stack)
    .. be32(image_end)                    -- reloc_start, of an empty table
    .. be32(0)                            -- reloc_count
    .. be32(FLAT_FLAG_RAM)
    .. be32(0)                            -- build_date
    .. string.rep(be32(0), 5)

if #header ~= FLAT_HDR_SIZE then
    die("built a %d-byte header, expected %d", #header, FLAT_HDR_SIZE)
end

local f = io.open(out, "wb") or die("cannot write %s", out)
f:write(header)
f:write(blob)
f:close()

print(string.format(
    "mkflt.lua: %s -- %d bytes (%d code+data, %d bss zeroed in place), " ..
    "%d relocs applied, entry 0x%x, stack %d",
    out, FLAT_HDR_SIZE + image_len, stored_end, bss_len, nrelocs, entry, stack))
