"""Headless tests for hypmon: the frame, the routing and the size answer.

Three things here have to be right or the program is unusable, and none of
them are visible from a screenshot:

  * Every rendered row must be EXACTLY the terminal width. One character
    over and the terminal wraps it, every row below shifts down, and the
    layout collapses -- which looks like a rendering bug anywhere but where
    it is.

  * The size answer must report the PANE, not the terminal. Reporting the
    terminal puts the guest's last line underneath hypmon's own chrome.

  * Keys must reach the guest byte for byte. Ctrl-C especially: the whole
    point of this program is that it does not eat it.

The terminal emulation itself is tested separately, in test_hypvt.py.
"""
import importlib.util
import os
import re
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


hypvt = load("hypvt")
hypmon = load("hypmon")

fails = []


def check(name, got, want):
    if got != want:
        fails.append(name)
        print("FAIL %s\n    got  %r\n    want %r" % (name, got, want))
    else:
        print("ok   %s" % name)


SGR = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


def plain(s):
    """The printable text of a rendered row."""
    return SGR.sub("", s)


class StubSerial:
    """Just enough serial port for the reset-board action.

    Ctrl-] b pulses RTS, so the action cannot be exercised at all without
    something to pulse -- and an unexercised action is how the sidebar ends
    up advertising a key that does nothing.
    """

    def __init__(self):
        self.dtr = self.rts = None

    def setDTR(self, v):
        self.dtr = v

    def setRTS(self, v):
        self.rts = v


class Fake(hypmon.Hypmon):
    """hypmon with no serial port, no console and no stdout."""

    def __init__(self, rows=30, cols=100):
        import threading
        self.ser = StubSerial()
        self.port, self.baud = "COMX", 4000000
        self.rows, self.cols = rows, cols
        self.lock = threading.RLock()
        self.screen = hypvt.Screen(self.pane_rows, self.pane_cols)
        self.stop = threading.Event()
        self.dirty = True
        self.rx = self.tx = self.rx_window = 0
        self.rate = self.peak_rate = 0.0
        self.backlog = self.peak_backlog = 0
        self.probes = 0
        self.carry = b""
        self.log = None
        self.logpath = None
        self.notice = ""
        self.notice_until = 0.0
        self.hyp = {}
        self.focus = "console"
        self.filter = ""
        self.scroll = 0
        self.timestamps = False
        self.wrap = False
        self.show_help = False
        self.stamps = {}
        self.size_changed = False
        self.prev = []
        self.sent = bytearray()

    def write_guest(self, data):
        self.sent += data

    def say(self, text, secs=4.0):
        self.notice = text


# ==== geometry ========================================================
f = Fake(rows=30, cols=100)
check("pane height leaves room for the chrome", f.pane_rows, 26)
check("pane width leaves room for the sidebar", f.pane_cols,
      100 - hypmon.SIDEBAR_W - 3)
check("pane origin is just inside the frame", f.pane_origin,
      (2, hypmon.SIDEBAR_W + 3))

# A tiny window must still produce a sane pane rather than a negative one.
f = Fake(rows=1, cols=1)
check("geometry clamps on a tiny window",
      (f.pane_rows >= 1, f.pane_cols >= 10), (True, True))

# ==== the frame is exactly as wide as the terminal ====================
# This is the invariant that keeps the layout from collapsing.
for rows, cols in ((30, 100), (24, 80), (60, 203), (10, 40), (8, 40),
                   (50, 41), (12, 250)):
    f = Fake(rows=rows, cols=cols)
    f.screen.feed(b"boot line one\r\nboot line two\r\n~ # ")
    f.hyp = {"traps": 481429, "kcyc": 132513, "dropped": 0, "kbd": 1}
    f.rx = 1234567
    bad = []
    for mode in ("log", "screen", "help", "filtered", "stamped", "wrapped"):
        f.show_help = (mode == "help")
        f.filter = "line" if mode == "filtered" else ""
        f.timestamps = (mode == "stamped")
        f.wrap = (mode == "wrapped")
        if mode == "screen":
            f.screen.feed(b"\x1b[?1049h\x1b[0;7m top \x1b[0mbody")
        rendered = f.render()
        if len(rendered) != f.rows:
            bad.append("%s: %d rows, want %d" % (mode, len(rendered), f.rows))
            break
        for i, line in enumerate(rendered):
            w = len(plain(line))
            if w != f.cols:
                bad.append("%s row %d width %d, want %d"
                           % (mode, i, w, f.cols))
                break
        if mode == "screen":
            f.screen.feed(b"\x1b[?1049l")
        if bad:
            break
    if bad:
        fails.append("frame width at %dx%d" % (rows, cols))
        print("FAIL frame at %dx%d: %s" % (rows, cols, bad[0]))
        break
else:
    print("ok   every row is exactly the terminal width, at 7 sizes x 6 modes")

# The frame must be a frame: corners, and a border down both edges.
f = Fake(rows=12, cols=60)
r = [plain(x) for x in f.render()]
check("top-left corner", r[0][0], "┌")
check("top-right corner", r[0][-1], "┐")
check("bottom-left corner", r[-1][0], "└")
check("bottom-right corner", r[-1][-1], "┘")
check("left edge is drawn on every middle row",
      all(x[0] in "│├└┌" for x in r), True)
check("right edge is drawn on every middle row",
      all(x[-1] in "│┤┘┐" for x in r), True)
check("the sidebar divider lines up with the separator",
      r[1][1 + hypmon.SIDEBAR_W], "│")
check("  ...and the separator has a tee under it",
      r[-3][1 + hypmon.SIDEBAR_W], "┴")

# ==== the size answer =================================================
f = Fake(rows=30, cols=100)
clean, n = f.split_probes(b"a\x1b[6nb")
check("the CPR probe is not fed to the screen", clean, b"ab")
check("  ...and is counted", n, 1)
f.answer_size(n)
check("CPR is answered with the pane size, not the terminal size",
      bytes(f.sent), b"\x1b[26;%dR" % f.pane_cols)

f = Fake(rows=57, cols=203)
_, n = f.split_probes(b"\x1b[18t")
f.answer_size(n)
check("the text-area probe gets the same answer",
      bytes(f.sent), b"\x1b[53;%dR" % f.pane_cols)

# The exact byte sequence `resize` sends. Only the probe is claimed; the
# cursor save and the move are the guest's business and belong to hypvt.
f = Fake(rows=30, cols=100)
probe = b"\x1b7\x1b[r\x1b[999;999H\x1b[6n"
clean, n = f.split_probes(probe)
check("resize probe: everything but the CPR is passed on",
      clean, b"\x1b7\x1b[r\x1b[999;999H")
check("resize probe answered exactly once", n, 1)

# Split at every possible boundary -- the 4 Mbps case. A probe cut in half
# must not be forwarded as text, and must still be answered once.
stream = b"before" + probe + b"after"
for cut in range(1, len(stream)):
    f = Fake(rows=30, cols=100)
    a, n1 = f.split_probes(stream[:cut])
    b, n2 = f.split_probes(stream[cut:])
    if a + b != b"before\x1b7\x1b[r\x1b[999;999Hafter" or n1 + n2 != 1:
        fails.append("probe split at %d" % cut)
        print("FAIL probe split at %d -> %r, %d probes"
              % (cut, a + b, n1 + n2))
        break
else:
    print("ok   every split point of a probe in traffic")

# A near-miss must not be swallowed.
f = Fake()
clean, n = f.split_probes(b"\x1b[6X\x1b[19t")
check("ESC[6X and ESC[19t are not probes", clean, b"\x1b[6X\x1b[19t")
check("  ...and answer nothing", n, 0)

# A lone trailing ESC is held, not passed through half-examined.
f = Fake()
clean, n = f.split_probes(b"data\x1b")
check("a trailing ESC is held back", clean, b"data")
clean2, _ = f.split_probes(b"[6n!")
check("  ...and resolves on the next read", clean2, b"!")

# ==== key routing =====================================================
f = Fake()
f.route(b"ls -l\r")
check("ordinary keys go straight to the guest", bytes(f.sent), b"ls -l\r")

f = Fake()
f.route(b"\x03")
check("Ctrl-C is forwarded, not eaten", bytes(f.sent), b"\x03")

f = Fake()
f.route(b"\x1e\x1f")
check("the hypervisor's own hotkeys pass through", bytes(f.sent),
      b"\x1e\x1f")

f = Fake()
f.route(b"\x1d\x1d")
check("Ctrl-] Ctrl-] sends one literal 0x1d", bytes(f.sent), b"\x1d")

f = Fake()
f.route(b"\x1dr")
check("Ctrl-] r sends `resize` to the guest", bytes(f.sent), b"\nresize\n")

f = Fake()
f.route(b"\x1ds")
check("Ctrl-] s sends Ctrl-^", bytes(f.sent), hypmon.HYP_STATS_KEY)

f = Fake()
f.route(b"\x1dk")
check("Ctrl-] k sends Ctrl-_", bytes(f.sent), hypmon.HYP_KBD_TEST_KEY)

f = Fake()
f.route(b"echo hi\r\x1dt")
check("text before a command is still sent", bytes(f.sent), b"echo hi\r")
check("  ...and the command took effect", f.timestamps, True)

# A prefix at the very end of a read: the command byte has not arrived yet,
# so it must be carried, not guessed at and not sent to the guest.
f = Fake()
unused, pending = f.route(b"abc\x1d")
check("a dangling Ctrl-] is carried to the next read", pending, b"\x1d")
check("  ...and the text before it went out", bytes(f.sent), b"abc")

f = Fake()
check("Ctrl-] q asks to quit", f.command("q"), False)
f = Fake()
check("an unknown command does not quit", f.command("Z"), True)
check("  ...and says so", "no such command" in f.notice, True)

# `/` opens the filter only at the start of a read; a slash inside a path
# must reach the guest or the console is unusable.
f = Fake()
f.route(b"/")
check("a leading / focuses the filter", f.focus, "filter")
check("  ...and is not sent", bytes(f.sent), b"")
f = Fake()
f.route(b"cat /etc/inittab\r")
check("a / inside a line is just a slash", bytes(f.sent),
      b"cat /etc/inittab\r")

f = Fake()
f.route(b"\x09")
check("Tab moves focus to the filter", f.focus, "filter")
f.route(b"\x09")
check("  ...and back", f.focus, "console")

# ==== the filter field ================================================
f = Fake()
f.focus = "filter"
left = f.handle_filter_key(b"boot")
check("typing edits the filter", f.filter, "boot")
check("  ...and nothing leaks to the guest", left, b"")
f.handle_filter_key(b"\x08")
check("backspace deletes", f.filter, "boo")
f.handle_filter_key(b"\x15")
check("Ctrl-U clears", f.filter, "")
f.filter = "x"
left = f.handle_filter_key(b"\x1bls\r")
check("Esc returns focus to the console", f.focus, "console")
check("  ...and hands the rest of the read to the guest", left, b"ls\r")

# ==== mouse ===========================================================
keys, events, tail = hypmon.split_mouse(b"a\x1b[<0;5;7Mb")
check("a click is pulled out of the key stream", keys, b"ab")
check("  ...and decoded", events, [(0, 5, 7, True)])
check("  ...with no tail", tail, b"")

# Arrow keys are CSI too and must survive.
keys, events, tail = hypmon.split_mouse(b"\x1b[A\x1b[B")
check("arrow keys are not mistaken for mouse reports",
      (keys, events), (b"\x1b[A\x1b[B", []))

# A report split across reads: forwarding half of one is a burst of junk in
# whatever the guest is running.
report = b"\x1b[<64;10;20M"
for cut in range(1, len(report)):
    k1, e1, t1 = hypmon.split_mouse(b"x" + report[:cut])
    k2, e2, t2 = hypmon.split_mouse(t1 + report[cut:] + b"y")
    if k1 + k2 != b"xy" or len(e1 + e2) != 1:
        fails.append("mouse split at %d" % cut)
        print("FAIL mouse split at %d -> keys %r events %r"
              % (cut, k1 + k2, e1 + e2))
        break
else:
    print("ok   every split point of a mouse report")

f = Fake(rows=30, cols=100)
f.screen.feed(b"\r\n".join(b"line %d" % i for i in range(200)))
f.handle_mouse((64, 50, 10, True))
check("the wheel scrolls back", f.scroll, 3)
f.handle_mouse((65, 50, 10, True))
check("  ...and forward again", f.scroll, 0)
f.handle_mouse((0, 50, 10, True))
check("clicking the pane focuses the console", f.focus, "console")
f.handle_mouse((0, 50, f.rows - 1, True))
check("clicking the filter row focuses the filter", f.focus, "filter")

# ==== log mode ========================================================
f = Fake(rows=14, cols=80)          # pane is 10 rows
for i in range(50):
    f.screen.feed(b"line %d\r\n" % i)
    f._restamp()
rows = f.log_rows(f.pane_rows, f.pane_cols)
check("log mode fills the pane", len(rows), 10)
check("log mode shows the tail", plain(rows[-1]).strip(), "line 49")

# The bug this replaced: stamps covered only the scrollback, so the pane --
# which shows the TAIL, i.e. the visible screen -- had a blank timestamp
# column always. Assert on what is rendered, not on the stamp count.
f.timestamps = True
rows = [plain(r) for r in f.log_rows(f.pane_rows, f.pane_cols)]
stamped = [r for r in rows if re.match(r"^\d\d:\d\d:\d\d ", r)]
check("every visible line in the tail carries a timestamp",
      len(stamped), len(rows))
check("  ...and the text is still there",
      rows[-1].split()[-1], "49")
f.timestamps = False

# Stability: the index a line is stamped under must not move when the
# screen scrolls, or every line gets re-stamped and the column becomes a
# clock rather than a record. On its own screen -- advancing f here would
# silently shift the fixture the scrollback checks below depend on.
g = Fake(rows=14, cols=80)
for i in range(50):
    g.screen.feed(b"line %d\r\n" % i)
    g._restamp()
before = dict(g.stamps)
g.screen.feed(b"more\r\n" * 5)
g._restamp()
kept = [k for k in before if g.stamps.get(k) == before[k]]
check("existing stamps survive further scrolling",
      len(kept), len(before))

f.scroll = 20
rows = f.log_rows(f.pane_rows, f.pane_cols)
check("scrolling back moves the window", plain(rows[-1]).strip(), "line 29")
f.scroll = 10 ** 6
rows = f.log_rows(f.pane_rows, f.pane_cols)
check("scrolling past the start is harmless", len(rows), 10)

f.scroll = 0
f.filter = "line 4"
rows = [plain(r).strip() for r in f.log_rows(f.pane_rows, f.pane_cols)]
shown = [r for r in rows if r]
check("the filter selects matching lines only",
      all("line 4" in r for r in shown), True)
check("  ...and there are the right number of them", len(shown), 10)

f.filter = "no such text anywhere"
rows = [plain(r) for r in f.log_rows(f.pane_rows, f.pane_cols)]
check("a filter matching nothing gives an empty pane",
      all(not r.strip() for r in rows), True)

# ==== screen mode =====================================================
f = Fake(rows=30, cols=100)
f.screen.feed(b"shell history\r\n")
f.screen.feed(b"\x1b[?1049h\x1b[2J\x1b[1;1H\x1b(0lqk\x1b(B\x1b[2;1Hbody")
check("the alt buffer switches the pane to screen mode",
      f.screen.in_alt, True)
rows = f.pane_contents(f.pane_rows, f.pane_cols)
check("box-drawing characters render", plain(rows[0])[:3], "┌─┐")
check("the app's second row renders", plain(rows[1])[:4], "body")
check("the header says which mode it is in", "screen" in f.header(), True)
f.screen.feed(b"\x1b[?1049l")
check("leaving the alt buffer goes back to log mode",
      "log" in f.header(), True)
rows = f.pane_contents(f.pane_rows, f.pane_cols)
check("  ...and the shell history is still there",
      plain(rows[0]).strip(), "shell history")

# Reverse video has to survive into the rendered row, or nano's title bar
# and shortcut bars vanish.
f = Fake(rows=30, cols=100)
f.screen.feed(b"\x1b[?1049h\x1b[0;7mGNU nano\x1b[0m")
rows = f.pane_contents(f.pane_rows, f.pane_cols)
check("reverse video survives rendering", "\x1b[7m" in rows[0], True)
check("  ...around the right text", plain(rows[0]).strip(), "GNU nano")

# ==== the cursor ======================================================
f = Fake(rows=30, cols=100)
f.screen.feed(b"\x1b[?1049h\x1b[5;10Hx")
r, c = f.cursor_position()
r0, c0 = f.pane_origin
check("in screen mode the cursor follows the guest's",
      (r - r0, c - c0), (4, 10))

f = Fake(rows=30, cols=100)
f.screen.feed(b"\x1b[?1049h\x1b[?25l")
check("a hidden guest cursor is hidden here too",
      f.cursor_position(), None)

f = Fake(rows=14, cols=80)
for i in range(5):
    f.screen.feed(b"line %d\r\n" % i)
f.screen.feed(b"~ # ")
pos = f.cursor_position()
check("in log mode the cursor is at the prompt", pos is not None, True)
f.scroll = 5
check("scrolled back, there is no cursor to show", f.cursor_position(), None)
f.scroll = 0
f.filter = "x"
check("filtered, there is no cursor to show", f.cursor_position(), None)

f = Fake()
f.focus = "filter"
f.filter = "abc"
r, c = f.cursor_position()
check("with the filter focused the cursor is in it", r, f.rows - 1)

# ==== scraping ========================================================
f = Fake()
f.scrape(b"HYP stats: 481429 traps, 132513 kcycles in the monitor\n")
f.scrape(b"  uart rx overruns: 0, tx dropped: 3392 | keyboards: 1\n")
check("scraped traps", f.hyp.get("traps"), 481429)
check("scraped kcycles", f.hyp.get("kcyc"), 132513)
check("scraped tx dropped", f.hyp.get("dropped"), 3392)
check("scraped overruns", f.hyp.get("overruns"), 0)
check("scraped keyboards", f.hyp.get("kbd"), 1)
check("dropped bytes reach the header", "tx dropped 3392" in f.header(), True)

f = Fake()
f.scrape(b"W: guest has taken no trap for 5 s -- wedged?")
check("the wedge warning is noticed", "wedged" in f.notice, True)

# ==== small helpers ===================================================
check("human(): bytes", hypmon.human(999), "999")
check("human(): thousands", hypmon.human(1500), "1.5k")
check("human(): tens of thousands", hypmon.human(45000), "45k")
check("human(): millions", hypmon.human(1234567), "1.2M")

ln = hypmon.Line(10)
ln.add("abcdefghijklmnop")
check("Line truncates to its width", len(plain(ln.build())), 10)
ln = hypmon.Line(10)
ln.add("ab", "\x1b[1m").pad()
check("Line pads to its width, ignoring SGR", len(plain(ln.build())), 10)
check("  ...and keeps the styling", "\x1b[1m" in ln.build(), True)

pairs = hypmon.wrap_pairs([("x" * 25, "12:00:00")], 10)
check("wrap splits a long line", [p[0] for p in pairs],
      ["x" * 10, "x" * 10, "x" * 5])
check("  ...and stamps only the first piece", [p[1] for p in pairs],
      ["12:00:00", "", ""])

ln = hypmon.Line(40)
built = hypmon.highlight(ln, "the boot line", "boot")
# Padded to the full row width on purpose: every pane row owes that promise
# to add_styled, which advances the frame's column count by exactly it.
check("highlight keeps the text intact", plain(built).rstrip(),
      "the boot line")
check("  ...and pads to the row width", len(plain(built)), 40)
check("  ...and marks the match", hypmon.S_FILTER in built, True)

# ==== the actions table drives the sidebar ============================
f = Fake(rows=30, cols=100)
side = [plain(t) for t, _ in f.sidebar_rows(f.pane_rows)]

# The sidebar has to name the modifier somewhere, or the keys read as bare
# letters and pressing one just sends it to the guest. "^]r" was the first
# attempt and nobody could tell what it meant.
check("the sidebar spells out the prefix",
      any("Ctrl+]" in row for row in side), True)
check("  ...and the footer does too", "Ctrl+]" in f.footer(), True)
check("  ...and the help page explains it is two presses",
      "TWO presses" in hypmon.HELP, True)

for a in hypmon.ACTIONS:
    if not any(row.strip().startswith(a.key + " ") and a.label in row
               for row in side):
        fails.append("sidebar missing %s" % a.key)
        print("FAIL sidebar has no row for Ctrl-] %s (%s)" % (a.key, a.label))
        break
else:
    print("ok   every action in the table has a sidebar row")

# Every action key must actually be handled, or the sidebar is advertising
# something that does nothing.
for a in hypmon.ACTIONS:
    if a.key == "q":
        continue
    f = Fake()
    if a.key == "b":
        f.reset_board()
        if (f.ser.dtr, f.ser.rts) != (False, False):
            fails.append("reset did not pulse RTS")
            print("FAIL Ctrl-] b did not pulse RTS low again")
            break
        continue
    f.command(a.key)
    if "no such command" in f.notice:
        fails.append("unhandled action %s" % a.key)
        print("FAIL Ctrl-] %s is in the sidebar but not handled" % a.key)
        break
else:
    print("ok   every advertised action is handled")

# A short window must not drop the sidebar rows it cannot fit onto the pane.
f = Fake(rows=8, cols=60)
side = f.sidebar_rows(f.pane_rows)
check("the sidebar never exceeds the pane height", len(side), f.pane_rows)

print()
print("%d failure(s)" % len(fails))
sys.exit(1 if fails else 0)
