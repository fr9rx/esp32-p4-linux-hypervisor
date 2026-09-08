#!/usr/bin/env python3
"""hypmon -- a framed terminal for the ESP32-P4 hypervisor's guest console.

Why this exists rather than `idf.py monitor` or picocom:

  * `idf.py monitor` eats Ctrl-C (it is its own quit key) and rewrites the
    byte stream looking for panic addresses to decode. Both are fatal for a
    full-screen guest program: the first means you cannot interrupt anything
    in the guest, and the second corrupts ncurses redraws.

  * A serial line carries no window size. There is no TIOCGWINSZ to answer,
    so `stty size` on the guest reads 0x0 and every ncurses program falls
    back to 24x80 in the corner of your screen. The fix is not on the guest:
    xterm's `resize` asks the terminal where the cursor lands after being
    sent to row 999, column 999, and something has to answer. This does.

  * At 4 Mbps a per-byte read loop in Python cannot keep up, and the guest's
    TX path drops bytes rather than waiting (HYP_UART_LOG_BLOCKING=0 in
    main/main.c), so falling behind does not just delay output -- it loses
    it, mid-escape-sequence, and the guest's screen tears.

Why it is framed, and why that needed a terminal emulator
---------------------------------------------------------
The first version of this program was a passthrough: guest bytes went
straight to the real terminal. That is why ncurses worked, and it is also
exactly why it could not have a status bar, a sidebar or a scrollback --
the guest emits absolute cursor moves like ESC[5;10H, so anything hypmon
drew was overwritten the moment nano started. A frame and a working editor
are in direct conflict under passthrough.

So hypmon no longer forwards; it interprets. hypvt.Screen keeps the guest's
screen as a grid of cells and the renderer paints that grid inside a panel.
The pane is a screen rather than a hole in the frame, which is what lets the
two coexist. `--raw` still gives you the old passthrough.

The pane has two modes, and the switch between them is free because the
guest announces it:

  log mode     -- the default. A line-oriented transcript with scrollback
                  and timestamps. Right for boot logs and the shell.
  screen mode  -- entered automatically when the guest switches to the
                  alternate screen buffer (ESC[?1049h, which is measurably
                  what nano does under TERM=xterm-256color). The app owns
                  the pane; scrollback stands down.

There is deliberately no "send" box. The console pane holds input, and
anything you type goes to the guest -- including Ctrl-C, and including the
hypervisor's own Ctrl-^ and Ctrl-_ hotkeys. Click the console to focus it.
Local commands are behind Ctrl-] , chosen because 0x1d is the only free
control code: 0x1e and 0x1f belong to the monitor on the board and must
pass through untouched.

Usage:
    python tools/hypmon.py [--port COM17] [--baud 4000000] [--log FILE]
    python tools/hypmon.py --raw
"""

import argparse
import ctypes
import os
import re
import shutil
import sys
import threading
import time

if os.name == "nt":
    import ctypes.wintypes as wintypes

try:
    import serial
except ImportError:
    sys.exit("hypmon: needs pyserial (pip install pyserial)")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hypvt                                              # noqa: E402

PREFIX = 0x1d                 # Ctrl-] -- see the module docstring
HYP_STATS_KEY = b"\x1e"       # Ctrl-^ -- handled by the monitor on the board
HYP_KBD_TEST_KEY = b"\x1f"    # Ctrl-_

FPS = 60.0
READ_CHUNK = 1 << 16

SIDEBAR_W = 20
CHROME_ROWS = 3               # top border, separator, bottom border
MIN_COLS = 40
MIN_ROWS = 8


# --------------------------------------------------------------------------
# Windows console: raw input, VT output.
# --------------------------------------------------------------------------
STD_INPUT_HANDLE = -10
STD_OUTPUT_HANDLE = -11

ENABLE_PROCESSED_INPUT = 0x0001
ENABLE_LINE_INPUT = 0x0002
ENABLE_ECHO_INPUT = 0x0004
ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200

ENABLE_PROCESSED_OUTPUT = 0x0001
ENABLE_WRAP_AT_EOL_OUTPUT = 0x0002
ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004


class Console:
    """Raw console on Windows; termios raw mode elsewhere."""

    def __init__(self):
        self.windows = os.name == "nt"
        self.saved = None
        self.k32 = (
            ctypes.WinDLL("kernel32", use_last_error=True)
            if self.windows else None
        )
        self.hin = None
        self.hout = None

    def __enter__(self):
        if self.windows:
            self.hin = self.k32.GetStdHandle(STD_INPUT_HANDLE)
            self.hout = self.k32.GetStdHandle(STD_OUTPUT_HANDLE)

            mi, mo = wintypes.DWORD(), wintypes.DWORD()
            self.k32.GetConsoleMode(self.hin, ctypes.byref(mi))
            self.k32.GetConsoleMode(self.hout, ctypes.byref(mo))

            self.saved = (
                mi.value,
                mo.value,
                self.k32.GetConsoleCP(),
                self.k32.GetConsoleOutputCP(),
            )

            self.k32.SetConsoleCP(65001)
            self.k32.SetConsoleOutputCP(65001)

            self.k32.SetConsoleMode(
                self.hin,
                ENABLE_VIRTUAL_TERMINAL_INPUT
            )

            self.k32.SetConsoleMode(
                self.hout,
                mo.value
                | ENABLE_PROCESSED_OUTPUT
                | ENABLE_WRAP_AT_EOL_OUTPUT
                | ENABLE_VIRTUAL_TERMINAL_PROCESSING
            )
        else:
            import termios
            import tty

            self.saved = termios.tcgetattr(sys.stdin.fileno())
            tty.setraw(sys.stdin.fileno())

        return self

    def __exit__(self, *exc):
        if not self.saved:
            return

        if self.windows:
            mi, mo, cp, ocp = self.saved
            self.k32.SetConsoleMode(self.hin, mi)
            self.k32.SetConsoleMode(self.hout, mo)
            self.k32.SetConsoleCP(cp)
            self.k32.SetConsoleOutputCP(ocp)
        else:
            import termios

            termios.tcsetattr(
                sys.stdin.fileno(),
                termios.TCSADRAIN,
                self.saved,
            )

    def read(self, n=1024):
        """Blocking read of raw key bytes."""
        if self.windows:
            buf = ctypes.create_string_buffer(n)
            got = wintypes.DWORD()

            ok = self.k32.ReadFile(
                self.hin,
                buf,
                n,
                ctypes.byref(got),
                None,
            )

            if not ok:
                return b""

            return buf.raw[:got.value]

        return os.read(sys.stdin.fileno(), n)


# --------------------------------------------------------------------------
# Styled line builder
# --------------------------------------------------------------------------
RESET = "\x1b[0m"


class Line:
    def __init__(self, width):
        self.width = width
        self.parts = []
        self.n = 0

    def add(self, text, sgr=""):
        if self.n >= self.width or not text:
            return self

        room = self.width - self.n

        if len(text) > room:
            text = text[:room]

        self.parts.append(sgr + text + RESET if sgr else text)
        self.n += len(text)
        return self

    def add_styled(self, text, width):
        """Append text that already contains SGR codes."""
        if self.n >= self.width:
            return self

        self.parts.append(text)
        self.n += width
        return self

    def pad(self, upto=None, ch=" ", sgr=""):
        upto = self.width if upto is None else min(upto, self.width)

        if upto > self.n:
            self.add(ch * (upto - self.n), sgr)

        return self

    def build(self):
        return "".join(self.parts)


# --------------------------------------------------------------------------
# Palette
# --------------------------------------------------------------------------
S_BORDER = "\x1b[38;5;240m"
S_TITLE = "\x1b[1m"
S_HEAD = "\x1b[1;36m"
S_DIM = "\x1b[38;5;245m"
S_KEY_HOT = "\x1b[1;33m"
S_LABEL = ""
S_FOCUS = "\x1b[1;32m"
S_NOTE = "\x1b[1;33m"


# --------------------------------------------------------------------------
# Actions
# --------------------------------------------------------------------------
class Action:
    def __init__(self, key, label, hot=False):
        self.key = key
        self.label = label
        self.hot = hot


ACTIONS = [
    Action("r", "resize guest"),
    Action("s", "trap stats"),
    Action("k", "keyboard test"),
    Action("b", "reset board", hot=True),
    Action("c", "clear log"),
    Action("l", "log to file"),
    Action("t", "timestamps"),
    Action("f", "follow tail"),
    Action("w", "wrap lines"),
    Action("d", "dump pane"),
    Action("?", "help"),
    Action("q", "quit"),
]


HELP = """\
 hypmon -- keys

 Every local command is TWO presses, not a chord:

     1. hold Ctrl and press  ]   (right square bracket)   -- then let go
     2. press the command letter on its own

 Nothing appears on screen after step 1; it is waiting for step 2.

 Anything else you type goes to the guest, including Ctrl-C and the
 hypervisor's own Ctrl-^ (trap stats) and Ctrl-_ (keyboard test).
 Ctrl-] is the prefix because 0x1d is the only control code the board
 does not already use for something.

   Ctrl-] r    run `resize` on the guest -- do this after login and after
               any host window resize. Nothing can push a size change down
               a serial line, so it has to be asked for.
   Ctrl-] s    hypervisor trap histogram     Ctrl-] k   keyboard self-test
   Ctrl-] b    reset the board               Ctrl-] c   clear the log
   Ctrl-] l    start/stop the log file       Ctrl-] t   timestamps
   Ctrl-] f    follow the tail again         Ctrl-] w   wrap long lines
   Ctrl-] d    write the pane to hypmon-dump.txt
   Ctrl-] q    quit                          Ctrl-] ?   this page
   Ctrl-] Ctrl-]   send a literal 0x1d to the guest

   Esc          back to the console
   PgUp/PgDn    scroll the log
   wheel        scroll
   click        focus the console

 Scrollback applies to log mode only. When a full-screen program starts,
 the guest switches to the alternate screen buffer and hypmon hands the
 whole pane over to it -- the header then says `screen`.

 Press any key to go back.
"""


# --------------------------------------------------------------------------
class Hypmon:
    def __init__(self, port, baud, logpath=None):
        self.port = port
        self.baud = baud

        self.ser = serial.Serial(
            port,
            baud,
            timeout=0.03,
            write_timeout=2.0,
        )

        try:
            self.ser.set_buffer_size(
                rx_size=1 << 20,
                tx_size=1 << 16,
            )
        except (AttributeError, OSError):
            pass

        self.rows, self.cols = self._term_size()

        self.lock = threading.RLock()
        self.screen = hypvt.Screen(
            self.pane_rows,
            self.pane_cols,
        )

        self.stop = threading.Event()
        self.dirty = True

        self.rx = self.tx = 0
        self.rx_window = 0
        self.rate = self.peak_rate = 0.0
        self.backlog = self.peak_backlog = 0
        self.probes = 0

        self.carry = b""

        self.log = open(logpath, "ab") if logpath else None
        self.logpath = logpath

        self.notice = ""
        self.notice_until = 0.0
        self.hyp = {}

        self.scroll = 0
        self.timestamps = False
        self.wrap = False
        self.show_help = False
        self.stamps = {}
        self.size_changed = False

        self.out = sys.stdout.buffer
        self.prev = []

    # ---- geometry ------------------------------------------------------
    def _term_size(self):
        cols, rows = shutil.get_terminal_size((100, 30))
        return max(MIN_ROWS, rows), max(MIN_COLS, cols)

    @property
    def pane_rows(self):
        """Rows inside the console panel."""
        return max(1, self.rows - CHROME_ROWS)

    @property
    def pane_cols(self):
        return max(10, self.cols - SIDEBAR_W - 3)

    @property
    def pane_origin(self):
        """(row, col) of the pane's top-left cell, 1-based, for ANSI."""
        return 2, SIDEBAR_W + 3

    def check_resize(self):
        rows, cols = self._term_size()

        if (rows, cols) == (self.rows, self.cols):
            return

        with self.lock:
            self.rows, self.cols = rows, cols
            self.screen.resize(
                self.pane_rows,
                self.pane_cols,
            )

            self.size_changed = True
            self.prev = []
            self.dirty = True

    # ---- guest -> us ---------------------------------------------------
    ANSWER_CPR = b"\x1b[6n"
    ANSWER_AREA = b"\x1b[18t"
    PROBES = (ANSWER_CPR, ANSWER_AREA)

    def split_probes(self, data):
        """Return (data_without_probes, n_probes)."""
        buf = self.carry + data
        self.carry = b""

        out = bytearray()
        n = 0
        i = 0

        while i < len(buf):
            if buf[i] != 0x1B:
                out.append(buf[i])
                i += 1
                continue

            rest = buf[i:]
            hit = None

            for probe in self.PROBES:
                if rest.startswith(probe):
                    hit = probe
                    break

            if hit:
                n += 1
                i += len(hit)
                continue

            if any(p.startswith(rest) for p in self.PROBES):
                self.carry = bytes(rest)
                return bytes(out), n

            out.append(buf[i])
            i += 1

        return bytes(out), n

    def answer_size(self, count=1):
        """Reply to a size probe with the PANE size."""
        rows, cols = self.pane_rows, self.pane_cols

        for _ in range(count):
            self.write_guest(
                b"\x1b[%d;%dR" % (rows, cols)
            )

        self.probes += count

    # ---- scraping ------------------------------------------------------
    RE_STATS = re.compile(rb"HYP stats: (\d+) traps, (\d+) kcycles")
    RE_DROP = re.compile(rb"tx dropped: (\d+)")
    RE_OVER = re.compile(rb"rx overruns: (\d+)")
    RE_KBD = re.compile(rb"keyboards: (\d+)")
    RE_WEDGE = re.compile(rb"no trap for (\d+) s")

    def scrape(self, data):
        m = self.RE_STATS.search(data)
        if m:
            self.hyp["traps"] = int(m.group(1))
            self.hyp["kcyc"] = int(m.group(2))

        m = self.RE_DROP.search(data)
        if m:
            self.hyp["dropped"] = int(m.group(1))

        m = self.RE_OVER.search(data)
        if m:
            self.hyp["overruns"] = int(m.group(1))

        m = self.RE_KBD.search(data)
        if m:
            self.hyp["kbd"] = int(m.group(1))

        m = self.RE_WEDGE.search(data)
        if m:
            self.say(
                "guest has taken no trap for %s s -- wedged?"
                % m.group(1).decode()
            )

    def say(self, text, secs=4.0):
        self.notice = text
        self.notice_until = time.time() + secs
        self.dirty = True

    # ---- us -> guest ---------------------------------------------------
    def write_guest(self, data):
        try:
            self.ser.write(data)
            self.tx += len(data)
        except (serial.SerialException, OSError) as e:
            self.say("write failed: %s" % e)

    # ---- serial pump ---------------------------------------------------
    def pump_serial(self):
        next_tick = time.time() + 1.0

        while not self.stop.is_set():
            try:
                waiting = self.ser.in_waiting
                data = self.ser.read(
                    max(1, min(waiting, READ_CHUNK))
                )
            except (serial.SerialException, OSError) as e:
                self.say("serial gone: %s" % e)
                self.stop.set()
                return

            self.backlog = waiting
            self.peak_backlog = max(
                self.peak_backlog,
                waiting,
            )

            if data:
                self.rx += len(data)
                self.rx_window += len(data)

                if self.log:
                    self.log.write(data)
                    self.log.flush()

                self.scrape(data)

                clean, probes = self.split_probes(data)

                if clean:
                    with self.lock:
                        self.screen.feed(clean)
                        self._restamp()

                self.dirty = True

                if probes:
                    self.answer_size(probes)

            now = time.time()

            if now >= next_tick:
                self.rate = self.rx_window
                self.peak_rate = max(
                    self.peak_rate,
                    self.rate,
                )
                self.rx_window = 0
                next_tick = now + 1.0
                self.dirty = True

    def _restamp(self):
        """Stamp every transcript line the first time it is seen."""
        now = time.strftime("%H:%M:%S")

        top = self.screen.lines_out

        for r in range(self.screen.rows):
            self.stamps.setdefault(
                top + r,
                now,
            )

        evicted = top - len(self.screen.scrollback)

        for i in range(len(self.screen.scrollback)):
            self.stamps.setdefault(
                evicted + i,
                now,
            )

        if len(self.stamps) > 20000:
            for key in [
                k for k in self.stamps
                if k < evicted
            ]:
                del self.stamps[key]

    # ---- rendering -----------------------------------------------------
    def log_pairs(self):
        """Return (text, stamp) for every log line."""
        with self.lock:
            return [
                (text, self.stamps.get(idx, ""))
                for text, idx
                in self.screen.all_lines_with_ids()
            ]

    def render(self):
        cols = self.cols
        pane_h = self.pane_rows
        pane_w = self.pane_cols

        out = []

        out.append(
            self.border_row(
                "┌",
                "┐",
                self.header(),
                S_HEAD,
            )
        )

        if self.show_help:
            body = self.help_rows(
                pane_h,
                cols - 2,
            )

            for r in range(pane_h):
                ln = Line(cols)
                ln.add("│", S_BORDER)
                ln.add(body[r])
                ln.pad(cols - 1)
                ln.add("│", S_BORDER)
                out.append(ln.build())

        else:
            side = self.sidebar_rows(pane_h)
            pane = self.pane_contents(
                pane_h,
                pane_w,
            )

            for r in range(pane_h):
                ln = Line(cols)

                ln.add("│", S_BORDER)

                text, style = side[r]
                ln.add(
                    text[:SIDEBAR_W],
                    style,
                )

                ln.pad(
                    1 + SIDEBAR_W
                )

                ln.add("│", S_BORDER)

                ln.add_styled(
                    pane[r] if r < len(pane) else "",
                    pane_w,
                )

                ln.pad(cols - 1)
                ln.add("│", S_BORDER)

                out.append(ln.build())

        ln = Line(cols)

        ln.add("├", S_BORDER)
        ln.pad(
            1 + SIDEBAR_W,
            "─",
            S_BORDER,
        )

        ln.add(
            "─" if self.show_help else "┴",
            S_BORDER,
        )

        ln.pad(
            cols - 1,
            "─",
            S_BORDER,
        )

        ln.add("┤", S_BORDER)
        out.append(ln.build())

        if self.notice and time.time() < self.notice_until:
            out.append(
                self.border_row(
                    "└",
                    "┘",
                    self.notice,
                    S_NOTE,
                )
            )
        else:
            out.append(
                self.border_row(
                    "└",
                    "┘",
                    self.footer(),
                    S_DIM,
                )
            )

        return out

    def border_row(self, left, right, text, style):
        cols = self.cols

        ln = Line(cols)

        ln.add(
            left + " ",
            S_BORDER,
        )

        ln.add(
            text[:max(0, cols - 4)],
            style,
        )

        ln.add(" ", S_BORDER)
        ln.pad(
            cols - 1,
            "─",
            S_BORDER,
        )

        ln.add(right, S_BORDER)

        return ln.build()

    def header(self):
        bits = [
            "hypmon",
            self.port,
            "%g Mbps" % (self.baud / 1e6),
        ]

        with self.lock:
            bits.append(
                "%dx%d" % (
                    self.pane_rows,
                    self.pane_cols,
                )
            )
            alt = self.screen.in_alt

        bits.append(
            "screen" if alt else "log"
        )

        if "traps" in self.hyp:
            bits.append(
                "%s traps" %
                human(self.hyp["traps"])
            )

        if self.hyp.get("dropped"):
            bits.append(
                "tx dropped %d" %
                self.hyp["dropped"]
            )

        if self.size_changed:
            bits.append(
                "window resized -- Ctrl-] r"
            )

        if self.log:
            bits.append("logging")

        return " · ".join(bits)

    def footer(self):
        left = (
            "Ctrl+] then ? for help · "
            "type to send"
        )

        right = [
            "%sB rx" % human(self.rx)
        ]

        with self.lock:
            nsb = len(self.screen.scrollback)
            lines_out = self.screen.lines_out
            limit = self.screen.scrollback_limit

        right.append(
            "%s lines" % human(lines_out)
        )

        if limit and nsb >= limit:
            right.append("scrollback full")

        if self.rate:
            right.append(
                "%sB/s" % human(
                    int(self.rate)
                )
            )

        right.append(
            "q %d/%d" %
            (self.backlog, self.peak_backlog)
        )

        if self.scroll:
            right.append(
                "SCROLLED BACK %d" %
                self.scroll
            )

        return (
            left
            + "    "
            + " · ".join(right)
        )

    def sidebar_rows(self, height):
        rows = [
            (
                " actions  Ctrl+] then:",
                S_TITLE,
            )
        ]

        for a in ACTIONS:
            if len(rows) >= height:
                break

            rows.append(
                (
                    "   %s   %s" %
                    (a.key, a.label),
                    S_KEY_HOT if a.hot else S_LABEL,
                )
            )

        for text in (
            "",
            " Esc  console",
            " PgUp scroll",
        ):
            if len(rows) >= height:
                break

            rows.append(
                (text, S_DIM)
            )

        while len(rows) < height:
            rows.append(("", ""))

        return rows

    def pane_contents(self, height, width):
        with self.lock:
            if self.screen.in_alt:
                return self.screen_rows(
                    height,
                    width,
                )

        return self.log_rows(
            height,
            width,
        )

    def screen_rows(self, height, width):
        out = []
        grid = self.screen.grid

        for y in range(height):
            if y >= len(grid):
                out.append(" " * width)
                continue

            row = grid[y]
            ln = Line(width)

            i = 0
            end = min(len(row), width)

            while i < end:
                cell = row[i]
                j = i + 1

                while (
                    j < end
                    and row[j].same_style(cell)
                ):
                    j += 1

                ln.add(
                    "".join(
                        c.ch
                        for c in row[i:j]
                    ),
                    sgr_for(cell),
                )

                i = j

            out.append(
                ln.pad().build()
            )

        return out

    def log_rows(self, height, width):
        pairs = self.log_pairs()
        stamp_w = 10 if self.timestamps else 0

        if self.wrap:
            pairs = wrap_pairs(
                pairs,
                width - stamp_w,
            )

        end = max(
            0,
            len(pairs) - self.scroll,
        )

        window = pairs[
            max(0, end - height):end
        ]

        out = []

        for text, stamp in window:
            ln = Line(width)

            if self.timestamps:
                ln.add(
                    "%-9s " % (stamp or ""),
                    S_DIM,
                )

            out.append(
                ln.add(text).pad().build()
            )

        while len(out) < height:
            out.append(" " * width)

        return out

    def help_rows(self, height, width):
        out = [
            Line(width).add(t).build()
            for t in HELP.split("\n")
        ]

        while len(out) < height:
            out.append("")

        return out[:height]

    # ---- painting ------------------------------------------------------
    def paint(self, force=False):
        rows = self.render()

        buf = ["\x1b[?25l"]

        for i, line in enumerate(rows):
            if (
                force
                or i >= len(self.prev)
                or self.prev[i] != line
            ):
                buf.append(
                    "\x1b[%d;1H%s\x1b[K" %
                    (i + 1, line)
                )

        self.prev = rows

        pos = self.cursor_position()

        if pos:
            buf.append(
                "\x1b[%d;%dH\x1b[?25h" %
                pos
            )
        else:
            buf.append(
                "\x1b[%d;1H" %
                self.rows
            )

        try:
            self.out.write(
                "".join(buf).encode(
                    "utf-8",
                    "replace",
                )
            )
            self.out.flush()
        except OSError:
            pass

    def cursor_position(self):
        if self.show_help:
            return None

        r0, c0 = self.pane_origin

        with self.lock:
            if self.screen.in_alt:
                if not self.screen.cursor_visible:
                    return None

                return (
                    r0 + min(
                        self.screen.cy,
                        self.pane_rows - 1,
                    ),
                    c0 + min(
                        self.screen.cx,
                        self.pane_cols - 1,
                    ),
                )

            cx = self.screen.cx

        if self.scroll or self.wrap:
            return None

        shown = min(
            len(self.log_pairs()),
            self.pane_rows,
        )

        if not shown:
            return None

        off = 10 if self.timestamps else 0

        return (
            r0 + shown - 1,
            c0 + off + min(
                cx,
                max(
                    0,
                    self.pane_cols - 1 - off,
                ),
            ),
        )

    # ---- commands ------------------------------------------------------
    def command(self, ch):
        """One Ctrl-] command. Returns False to quit."""

        if ch == "q":
            return False

        if ch == "r":
            self.write_guest(
                b"\nresize\n"
            )

            self.size_changed = False

            self.say(
                "sent `resize` -- the pane is %dx%d"
                % (
                    self.pane_rows,
                    self.pane_cols,
                )
            )

        elif ch == "s":
            self.write_guest(
                HYP_STATS_KEY
            )

            self.say(
                "asked the monitor for its trap histogram"
            )

        elif ch == "k":
            self.write_guest(
                HYP_KBD_TEST_KEY
            )

            self.say(
                "asked the monitor for the keyboard self-test"
            )

        elif ch == "b":
            self.reset_board()

        elif ch == "c":
            with self.lock:
                del self.screen.scrollback[:]
                self.stamps = {}
                self.screen.lines_out = 0

            self.scroll = 0
            self.say("log cleared")

        elif ch == "l":
            self.toggle_log()

        elif ch == "t":
            self.timestamps = not self.timestamps
            self.say(
                "timestamps %s" %
                onoff(self.timestamps)
            )

        elif ch == "f":
            self.scroll = 0
            self.say("following the tail")

        elif ch == "w":
            self.wrap = not self.wrap
            self.say(
                "line wrap %s" %
                onoff(self.wrap)
            )

        elif ch == "d":
            self.dump()

        elif ch in ("?", "h"):
            self.show_help = True
            self.prev = []

        else:
            self.say(
                "no such command: Ctrl-] %s   "
                "(Ctrl-] ? for help)"
                % (
                    repr(ch)
                    if not ch.isprintable()
                    else ch
                )
            )

        self.dirty = True
        return True

    def reset_board(self):
        """Pulse RTS the way esptool does."""
        try:
            self.ser.setDTR(False)
            self.ser.setRTS(True)
            time.sleep(0.15)
            self.ser.setRTS(False)
            self.say("board reset")
        except (serial.SerialException, OSError) as e:
            self.say(
                "reset failed: %s" % e
            )

    def toggle_log(self):
        if self.log:
            self.log.close()
            self.log = None

            self.say(
                "stopped logging to %s" %
                self.logpath
            )
            return

        path = self.logpath or "hypmon.log"

        try:
            self.log = open(
                path,
                "ab",
            )

            self.logpath = path

            self.say(
                "logging to %s" %
                path
            )

        except OSError as e:
            self.say(
                "cannot open %s: %s" %
                (path, e)
            )

    def dump(self):
        path = "hypmon-dump.txt"

        try:
            with self.lock:
                body = (
                    self.screen.text()
                    if self.screen.in_alt
                    else "\n".join(
                        self.screen.all_lines()
                    )
                )

            with open(
                path,
                "w",
                encoding="utf-8",
            ) as fh:
                fh.write(body + "\n")

            self.say(
                "wrote %s (%d bytes)" %
                (path, len(body) + 1)
            )

        except OSError as e:
            self.say(
                "dump failed: %s" % e
            )

    # ---- input ---------------------------------------------------------
    def pump_keys(self, console):
        """Read keys and route them."""

        pending = b""

        while not self.stop.is_set():
            data = console.read(512)

            if not data:
                time.sleep(0.05)
                continue

            if pending:
                data = pending + data
                pending = b""

            if self.show_help:
                self.show_help = False
                self.prev = []
                self.dirty = True
                continue

            data, events, tail = split_mouse(data)

            for ev in events:
                self.handle_mouse(ev)

            if tail:
                pending = tail

                if not data:
                    continue

            data, pending_extra = self.route(data)

            if pending_extra is not None:
                pending = pending_extra

            if self.stop.is_set():
                return

    def route(self, data):
        """Split a read into guest bytes and local commands."""

        out = bytearray()
        i = 0

        while i < len(data):
            b = data[i]

            if b == PREFIX:
                if i + 1 >= len(data):
                    if out:
                        self.write_guest(
                            bytes(out)
                        )

                    return b"", data[i:]

                nxt = data[i + 1]

                if nxt == PREFIX:
                    out.append(PREFIX)

                else:
                    if out:
                        self.write_guest(
                            bytes(out)
                        )
                        out = bytearray()

                    if not self.command(
                        chr(nxt)
                    ):
                        self.stop.set()
                        return b"", None

                i += 2
                continue

            out.append(b)
            i += 1

        if out:
            self.write_guest(
                bytes(out)
            )

        return b"", None

    def handle_mouse(self, ev):
        button, col, row, press = ev

        if button == 64:
            self.scroll = min(
                self.scroll + 3,
                self.max_scroll(),
            )
            self.dirty = True
            return

        if button == 65:
            self.scroll = max(
                0,
                self.scroll - 3,
            )
            self.dirty = True
            return

        if press:
            self.dirty = True

    def max_scroll(self):
        with self.lock:
            n = (
                len(self.screen.scrollback)
                + self.screen.rows
            )

        return max(
            0,
            n - self.pane_rows,
        )

    # ---- main loop -----------------------------------------------------
    def run(self, console):
        threading.Thread(
            target=self.pump_serial,
            daemon=True,
        ).start()

        threading.Thread(
            target=self.pump_keys,
            args=(console,),
            daemon=True,
        ).start()

        self.out.write(
            b"\x1b[?1049h\x1b[2J"
        )

        self.out.write(
            b"\x1b[?1000h\x1b[?1006h"
        )

        self.out.flush()

        self.paint(force=True)

        try:
            period = 1.0 / FPS

            while not self.stop.is_set():
                time.sleep(period)

                self.check_resize()

                if self.dirty:
                    self.dirty = False
                    self.paint()

        finally:
            self.out.write(
                b"\x1b[?1006l\x1b[?1000l"
            )

            self.out.write(
                b"\x1b[?25h\x1b[?1049l"
            )

            self.out.flush()

    def close(self):
        self.stop.set()

        if self.log:
            self.log.close()

        try:
            self.ser.close()
        except Exception:
            pass


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def sgr_for(cell):
    codes = []

    if cell.attr & hypvt.BOLD:
        codes.append("1")

    if cell.attr & hypvt.DIM:
        codes.append("2")

    if cell.attr & hypvt.ITALIC:
        codes.append("3")

    if cell.attr & hypvt.UNDERLINE:
        codes.append("4")

    if cell.attr & hypvt.REVERSE:
        codes.append("7")

    if cell.fg >= 0:
        codes.append(
            "38;5;%d" % cell.fg
        )

    if cell.bg >= 0:
        codes.append(
            "48;5;%d" % cell.bg
        )

    return (
        "\x1b[%sm" % ";".join(codes)
        if codes
        else ""
    )


def human(n):
    """Byte and line counts."""
    n = float(n)

    for unit in ("", "k", "M", "G"):
        if abs(n) < 1000:
            if unit and n < 10:
                return "%.1f%s" % (
                    n,
                    unit,
                )

            return "%d%s" % (
                n,
                unit,
            )

        n /= 1000.0

    return "%dT" % n


def onoff(flag):
    return "on" if flag else "off"


def wrap_pairs(pairs, width):
    """Split over-long lines, keeping the stamp on the first piece only."""
    width = max(8, width)
    out = []

    for text, stamp in pairs:
        if len(text) <= width:
            out.append(
                (text, stamp)
            )
            continue

        first = True

        for i in range(0, len(text), width):
            out.append(
                (
                    text[i:i + width],
                    stamp if first else "",
                )
            )
            first = False

    return out


MOUSE_RE = re.compile(
    rb"\x1b\[<(\d+);(\d+);(\d+)([Mm])"
)

MOUSE_PARTIAL = re.compile(
    rb"\x1b(\[(<[\d;]*)?)?$"
)


def split_mouse(data):
    """Pull SGR mouse reports out of the key stream."""
    events = []
    out = bytearray()
    i = 0

    while i < len(data):
        if data[i] != 0x1B:
            out.append(data[i])
            i += 1
            continue

        m = MOUSE_RE.match(
            data,
            i,
        )

        if m:
            button = int(
                m.group(1)
            )

            events.append(
                (
                    button,
                    int(m.group(2)),
                    int(m.group(3)),
                    m.group(4) == b"M",
                )
            )

            i = m.end()
            continue

        rest = bytes(data[i:])

        if (
            len(rest) <= 16
            and MOUSE_PARTIAL.match(rest)
        ):
            return (
                bytes(out),
                events,
                rest,
            )

        out.append(data[i])
        i += 1

    return (
        bytes(out),
        events,
        b"",
    )


# --------------------------------------------------------------------------
# raw passthrough
# --------------------------------------------------------------------------
def run_raw(port, baud, logpath):
    """No frame, no emulator -- bytes straight through."""
    ser = serial.Serial(
        port,
        baud,
        timeout=0.03,
    )

    try:
        ser.set_buffer_size(
            rx_size=1 << 20
        )
    except (AttributeError, OSError):
        pass

    log = (
        open(logpath, "ab")
        if logpath
        else None
    )

    out = sys.stdout.buffer
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try:
                n = ser.in_waiting
                data = ser.read(
                    max(
                        1,
                        min(
                            n,
                            READ_CHUNK,
                        ),
                    )
                )
            except (serial.SerialException, OSError):
                stop.set()
                return

            if data:
                if log:
                    log.write(data)

                out.write(data)
                out.flush()

    sys.stdout.write(
        "hypmon --raw on %s at %d. "
        "Ctrl-] q quits.\r\n"
        % (port, baud)
    )
    sys.stdout.flush()

    threading.Thread(
        target=reader,
        daemon=True,
    ).start()

    with Console() as console:
        try:
            while not stop.is_set():
                data = console.read(512)

                if not data:
                    time.sleep(0.05)
                    continue

                buf = bytearray()
                i = 0

                while i < len(data):
                    if (
                        data[i] == PREFIX
                        and i + 1 < len(data)
                    ):
                        if data[i + 1] == PREFIX:
                            buf.append(PREFIX)

                        elif (
                            chr(data[i + 1])
                            == "q"
                        ):
                            stop.set()
                            break

                        i += 2
                        continue

                    buf.append(data[i])
                    i += 1

                if buf:
                    ser.write(bytes(buf))

        finally:
            stop.set()

            if log:
                log.close()

            ser.close()


# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description=(
            "Framed terminal for the "
            "ESP32-P4 hypervisor guest."
        )
    )

    ap.add_argument(
        "--port",
        default="COM17",
    )

    ap.add_argument(
        "--baud",
        type=int,
        default=4000000,
        help=(
            "4 Mbps is the Kconfig ceiling "
            "and what the firmware is built for"
        ),
    )

    ap.add_argument(
        "--log",
        default=None,
        help="append the raw stream to this file",
    )

    ap.add_argument(
        "--raw",
        action="store_true",
        help=(
            "no frame: pass bytes straight "
            "through, as the first version did"
        ),
    )

    args = ap.parse_args(argv)

    if args.raw:
        run_raw(
            args.port,
            args.baud,
            args.log,
        )
        return 0

    try:
        mon = Hypmon(
            args.port,
            args.baud,
            args.log,
        )

    except (serial.SerialException, OSError) as e:
        sys.stderr.write(
            "hypmon: cannot open %s: %s\n"
            % (args.port, e)
        )
        return 1

    try:
        with Console() as console:
            mon.run(console)

    finally:
        mon.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
