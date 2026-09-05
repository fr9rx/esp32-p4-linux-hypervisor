"""Headless tests for the terminal emulator.

A terminal emulator is mostly edge cases, and the ones that bite are the
ones you cannot see: a sequence split across two serial reads, a colour
parameter mistaken for an attribute, a charset left switched on. All of
these are silent -- the screen just looks slightly wrong, and you blame the
guest.

The nano and nctest byte patterns here are real: they were captured off the
board, not invented.
"""
import importlib.util
import os
import sys

# This file is full of box-drawing characters and the Windows console
# defaults to cp1252, which cannot encode them -- the tests would die in
# print() rather than in the code under test.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "hypvt", os.path.join(HERE, "hypvt.py"))
hypvt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hypvt)

Screen = hypvt.Screen
fails = []


def check(name, got, want):
    if got != want:
        fails.append(name)
        print("FAIL %s\n    got  %r\n    want %r" % (name, got, want))
    else:
        print("ok   %s" % name)


# --- the basics -------------------------------------------------------
s = Screen(5, 10)
s.feed(b"hello")
check("plain text lands", s.line(0), "hello")
check("cursor advanced", (s.cy, s.cx), (0, 5))

s = Screen(5, 10)
s.feed(b"one\r\ntwo")
check("CRLF starts a new row", (s.line(0), s.line(1)), ("one", "two"))

s = Screen(5, 10)
s.feed(b"abc\rX")
check("CR returns to column 0", s.line(0), "Xbc")

s = Screen(5, 10)
s.feed(b"abc\x08\x08X")
check("BS moves back without erasing", s.line(0), "aXc")

s = Screen(5, 20)
s.feed(b"a\tb")
check("HT goes to the next multiple of 8", s.line(0), "a       b")

# Writing the last column must NOT scroll until there is another character:
# a shell prompt that exactly fills the width would otherwise scroll a line
# every time you pressed a key.
s = Screen(3, 5)
s.feed(b"abcde")
check("last column does not wrap yet", (s.cy, s.line(0)), (0, "abcde"))
s.feed(b"f")
check("  ...it wraps on the next character", (s.cy, s.line(1)), (1, "f"))

s = Screen(3, 5)
s.feed(b"\x1b[?7l" + b"abcdefgh")
check("autowrap off overwrites the last column", (s.cy, s.line(0)),
      (0, "abcdh"))

# --- cursor addressing ------------------------------------------------
s = Screen(10, 20)
s.feed(b"\x1b[5;10Hx")
check("CUP is 1-based", (s.cy, s.cx - 1), (4, 9))

s = Screen(10, 20)
s.feed(b"\x1b[99;99Hx")
check("CUP clamps to the screen", (s.cy, s.cx), (9, 19))

s = Screen(10, 20)
s.feed(b"\x1b[Hx")
check("bare CUP is home", (s.cy, s.line(0)), (0, "x"))

s = Screen(10, 20)
s.feed(b"\x1b[3;3H\x1b[2A\x1b[4C")
check("relative moves", (s.cy, s.cx), (0, 6))

# --- erase ------------------------------------------------------------
s = Screen(3, 10)
s.feed(b"abcdef\x1b[4G\x1b[K")
check("EL 0 clears to end of line", s.line(0), "abc")

s = Screen(3, 10)
s.feed(b"abcdef\x1b[4G\x1b[1K")
check("EL 1 clears to start of line", s.line(0), "    ef")

s = Screen(3, 10)
s.feed(b"row0\r\nrow1\r\nrow2\x1b[2;1H\x1b[J")
check("ED 0 clears down", (s.line(0), s.line(1), s.line(2)),
      ("row0", "", ""))

s = Screen(3, 10)
s.feed(b"row0\r\nrow1\x1b[2J")
check("ED 2 clears everything", s.text(), "\n\n")
check("ED 2 keeps the text in scrollback", s.scrollback, ["row0", "row1"])

# --- insert / delete --------------------------------------------------
s = Screen(3, 10)
s.feed(b"abcdef\x1b[1;3H\x1b[2P")
check("DCH deletes characters", s.line(0), "abef")

s = Screen(3, 10)
s.feed(b"abcdef\x1b[1;3H\x1b[2@")
check("ICH inserts blanks", s.line(0), "ab  cdef")

s = Screen(4, 10)
s.feed(b"a\r\nb\r\nc\x1b[2;1H\x1b[M")
check("DL deletes a line", [s.line(i) for i in range(4)],
      ["a", "c", "", ""])

s = Screen(4, 10)
s.feed(b"a\r\nb\x1b[2;1H\x1b[L")
check("IL inserts a line", [s.line(i) for i in range(4)],
      ["a", "", "b", ""])

# --- scrolling and DECSTBM -------------------------------------------
s = Screen(3, 10)
s.feed(b"1\r\n2\r\n3\r\n4")
check("scrolling drops the top row", [s.line(i) for i in range(3)],
      ["2", "3", "4"])
check("the dropped row went to scrollback", s.scrollback, ["1"])

# nano's real region. Content outside it must stay put -- that is the whole
# point of a region, and it is what hypmon relies on NOT to rely on.
s = Screen(6, 10)
s.feed(b"top\r\n\x1b[2;5r\x1b[2;1Ha\r\nb\r\nc\r\nd\r\ne")
check("region keeps row 0", s.line(0), "top")
check("region scrolls only inside itself",
      [s.line(i) for i in range(1, 6)], ["b", "c", "d", "e", ""])
check("scroll inside a region is not scrollback", s.scrollback, [])

s = Screen(6, 10)
s.feed(b"\x1b[2;5r")
check("DECSTBM homes the cursor to the region top", (s.cy, s.cx), (1, 0))
s.feed(b"\x1b[r")
check("bare ESC[r resets the region", (s.top, s.bot), (0, 5))

# --- SGR --------------------------------------------------------------
s = Screen(3, 10)
s.feed(b"\x1b[1;31mR\x1b[0mN")
check("bold+colour is recorded", (s.grid[0][0].attr & hypvt.BOLD,
                                 s.grid[0][0].fg), (hypvt.BOLD, 1))
check("SGR 0 resets", (s.grid[0][1].attr, s.grid[0][1].fg),
      (0, hypvt.DEFAULT_FG))

# nano's actual attribute sequence.
s = Screen(3, 20)
s.feed(b"\x1b[0;7mtitle\x1b[0m")
check("nano's ESC[0;7m gives reverse video",
      s.grid[0][0].attr & hypvt.REVERSE, hypvt.REVERSE)

# 256-colour: the arguments must be consumed, or the 5 becomes "blink" and
# the colour index becomes some other attribute entirely.
s = Screen(3, 10)
s.feed(b"\x1b[38;5;196mX")
check("256-colour fg consumed correctly",
      (s.grid[0][0].fg, s.grid[0][0].attr), (196, 0))
s = Screen(3, 10)
s.feed(b"\x1b[48;2;10;20;30mX")
check("truecolour bg does not leak into attrs", s.grid[0][0].attr, 0)

# --- the DEC line-drawing charset ------------------------------------
# This is nctest's frame. Without the charset it renders as "lqqk".
s = Screen(3, 10)
s.feed(b"\x1b(0lqqk\x1b(B")
check("ESC(0 draws box characters", s.line(0), "┌──┐")
check("ESC(B returns to ASCII", s.graphics, False)
s.feed(b"lqqk")
check("  ...and letters are letters again", s.line(0), "┌──┐lqqk")

s = Screen(3, 10)
s.feed(b"\x0elqk\x0f" + b"lqk")
check("SO/SI switch the charset too", s.line(0), "┌─┐lqk")

# The real nctest fragment captured from the board.
s = Screen(5, 40)
s.feed(b"\x1b(B\x1b(0lqk\x1b(B\x1b(0x\x1b(B")
check("captured nctest bytes render as a frame", s.line(0), "┌─┐│")

# --- REP --------------------------------------------------------------
# ncurses draws a horizontal rule as one character plus a repeat count, so
# a box without REP renders as three characters. This was found on the
# board, not in the spec.
s = Screen(3, 20)
s.feed(b"x[4b")
check("REP repeats the last character", s.line(0), "xxxxx")

# The real ncurses pattern: corner, one rule character, repeat, corner.
s = Screen(3, 20)
s.feed(b"(0lq[5bk(B")
check("REP draws an ncurses box rule", s.line(0), "┌──────┐")

s = Screen(3, 20)
s.feed(b"[3b")
check("REP with nothing to repeat is harmless", s.line(0), "")

s = Screen(3, 10)
s.feed(b"a[9999b")
check("a huge repeat count cannot run away",
      (len(s.line(0)), s.cy < s.rows), (10, True))

s = Screen(3, 20)
s.feed(b"ab[2b")
check("REP repeats the LAST character only", s.line(0), "abbb")

# --- IRM --------------------------------------------------------------
# nano sends ESC[4l on startup. Replace mode is the default here, so the
# sequence is a no-op -- but a deliberate one rather than an ignored one.
s = Screen(3, 10)
s.feed(b"abcd[1;2H[4hXY")
check("insert mode shifts the rest of the row right", s.line(0), "aXYbcd")
s = Screen(3, 10)
s.feed(b"abcd[1;2H[4lX")
check("replace mode overwrites, and is the default", s.line(0), "aXcd")
s = Screen(3, 10)
s.feed(b"[4h[4labcd")
check("IRM off after on behaves as replace", s.line(0), "abcd")

# --- the alternate screen --------------------------------------------
s = Screen(5, 10)
s.feed(b"shell\r\nhistory\r\n")
s.feed(b"\x1b[?1049h")
check("alt screen starts blank", s.text().strip(), "")
check("alt screen is flagged", s.in_alt, True)
s.feed(b"editor")
check("app draws into the alt buffer", s.line(0), "editor")
check("log mode ignores alt content", s.all_lines(), ["shell", "history"])
s.feed(b"\x1b[?1049l")
check("leaving restores the primary buffer",
      [s.line(0), s.line(1)], ["shell", "history"])
check("  ...and clears the flag", s.in_alt, False)

# Scrolling inside the alt buffer must not pollute the log with an editor's
# redraw frames.
s = Screen(3, 10)
s.feed(b"real\r\n\x1b[?1049h" + b"a\r\nb\r\nc\r\nd\r\ne")
check("alt-screen scroll adds no scrollback", s.scrollback, [])

# --- save / restore ---------------------------------------------------
s = Screen(5, 10)
s.feed(b"\x1b[3;4H\x1b7\x1b[1;1H\x1b8")
check("ESC7/ESC8 round-trip the cursor", (s.cy, s.cx), (2, 3))
s = Screen(5, 10)
s.feed(b"\x1b[2;2H\x1b[s\x1b[5;5H\x1b[u")
check("CSI s/u round-trip the cursor", (s.cy, s.cx), (1, 1))

# --- split feeds: the 4 Mbps case ------------------------------------
# Every one of these arrives in pieces at some point. Feeding a stream one
# byte at a time must produce exactly the same screen as feeding it whole.
STREAMS = [
    b"\x1b[2J\x1b[1;1Hhello\x1b[3;5Hworld\x1b[0;7mrev\x1b[0m",
    b"\x1b(0lqqk\x1b(B text \x1b[?1049h\x1b[2;5reditor\x1b[?1049l",
    b"a\r\nb\r\nc\x1b[38;5;9mX\x1b[1;24rY",
    b"\x1b]0;a title\x07after",
]
for idx, stream in enumerate(STREAMS):
    whole = Screen(6, 20)
    whole.feed(stream)
    piece = Screen(6, 20)
    for i in range(len(stream)):
        piece.feed(stream[i:i + 1])
    if whole.text() != piece.text():
        fails.append("split stream %d" % idx)
        print("FAIL byte-by-byte stream %d differs" % idx)
        print("   whole: %r" % whole.text())
        print("   split: %r" % piece.text())
        break
else:
    print("ok   all %d streams identical fed whole or one byte at a time"
          % len(STREAMS))

# Every split point, not just every byte boundary in sequence.
stream = b"x\x1b[5;5H\x1b(0q\x1b(B\x1b[0;7mZ\x1b[0m"
ref = Screen(6, 20)
ref.feed(stream)
for cut in range(1, len(stream)):
    two = Screen(6, 20)
    two.feed(stream[:cut])
    two.feed(stream[cut:])
    if two.text() != ref.text():
        fails.append("two-way split at %d" % cut)
        print("FAIL split at %d\n   %r\n   %r" % (cut, two.text(), ref.text()))
        break
else:
    print("ok   every two-way split point agrees with the whole stream")

# --- UTF-8 across a read boundary ------------------------------------
s = Screen(3, 10)
s.feed(b"a\xe2\x94")          # first two bytes of a box-drawing character
check("incomplete UTF-8 is held", s.line(0), "a")
s.feed(b"\x80b")              # ...completing U+2500
check("  ...and completes on the next feed", s.line(0), "a─b")

s = Screen(3, 10)
for byte in "héllo ─ ok".encode("utf-8"):
    s.feed(bytes([byte]))
check("UTF-8 byte-at-a-time", s.line(0), "héllo ─ ok")

# --- robustness -------------------------------------------------------
# Unknown sequences are swallowed, never printed. A terminal that echoes
# what it does not understand makes guest bugs out of its own gaps.
s = Screen(3, 20)
s.feed(b"a\x1b[>4;2mb\x1b[?2004hc")
check("unknown sequences leave no litter", s.line(0), "abc")

s = Screen(3, 20)
s.feed(b"a\x00\x01\x7fb")
check("stray C0 and DEL are dropped", s.line(0), "ab")

s = Screen(3, 20)
s.feed(b"\x1b]0;" + b"x" * 900 + b"\x07tail")
check("a runaway OSC cannot grow without bound", len(s._osc) < 600, True)

s = Screen(4, 10)
s.feed(b"\x1b[100;200r" + b"ok")
check("an impossible region is ignored", (s.top, s.bot), (0, 3))

# --- resize -----------------------------------------------------------
s = Screen(5, 20)
s.feed(b"\x1b[3;3Hkeep")
s.resize(10, 40)
check("resize preserves content", s.line(2), "  keep")
check("resize resets the region", (s.top, s.bot), (0, 9))
s.resize(2, 5)
check("shrink clamps the cursor", (s.cy < 2, s.cx < 5), (True, True))

# --- counters ---------------------------------------------------------
s = Screen(3, 10)
for i in range(10):
    s.feed(b"line%d\r\n" % i)
check("lines_out counts what scrolled off", s.lines_out, 8)
check("scrollback holds them", s.scrollback[:2], ["line0", "line1"])

s = Screen(3, 10, scrollback=4)
for i in range(20):
    s.feed(b"l%d\r\n" % i)
check("scrollback is capped", len(s.scrollback), 4)
check("  ...keeping the newest", s.scrollback[-1], "l17")

print()
print("%d failure(s)" % len(fails))
sys.exit(1 if fails else 0)
