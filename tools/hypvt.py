"""A terminal screen, as a data structure.

hypmon needs to draw a frame around the guest's console. A passthrough
terminal cannot have a frame: the guest emits absolute cursor moves like
ESC[5;10H, and those land on the real screen, so any border gets overwritten
the moment ncurses starts. The fix is to stop forwarding and start
interpreting -- keep the guest's screen here as a grid of cells, and let the
renderer paint that grid wherever it likes.

Everything in this module is pure: bytes in, grid out, no I/O and no
terminal. That is what makes it testable, which matters because a terminal
emulator is mostly edge cases.

Scope is deliberately "what the guest actually emits", established by
measurement rather than by reading the xterm spec end to end:

  * busybox/ash and the kernel: CR, LF, BS, HT, BEL, SGR, EL, ED, CUP.
  * nano: the alternate screen buffer, DECSTBM, its own SGR (ESC[0;7m), and
    cursor save/restore.
  * nctest: the DEC special graphics charset via ESC(0 / SO / SI -- its box
    frame arrives as the letters lqkxmjtuvwn and renders as mojibake if you
    ignore the charset. This is not optional; it was the first thing that
    looked broken.
  * nctest again: REP (ESC[<n>b). ncurses draws a horizontal rule as one
    character plus a repeat count, so without REP a 77-column box renders
    as three characters. Found by running it on the board, not by reading
    the spec -- which is the argument for testing against real bytes.

Unknown sequences are swallowed rather than printed. A terminal that prints
the escapes it does not understand produces garbage that looks like a guest
bug, and that is a bad way to spend an evening.
"""

# --- attributes -------------------------------------------------------
# Packed into one small int per cell so a screen is cheap to copy and diff.
BOLD = 1 << 0
DIM = 1 << 1
ITALIC = 1 << 2
UNDERLINE = 1 << 3
BLINK = 1 << 4
REVERSE = 1 << 5

DEFAULT_FG = -1
DEFAULT_BG = -1


class Cell(object):
    """One character position. Mutable, and reused -- see Screen.clear_cell."""

    __slots__ = ("ch", "attr", "fg", "bg")

    def __init__(self, ch=" ", attr=0, fg=DEFAULT_FG, bg=DEFAULT_BG):
        self.ch = ch
        self.attr = attr
        self.fg = fg
        self.bg = bg

    def blank(self):
        self.ch = " "
        self.attr = 0
        self.fg = DEFAULT_FG
        self.bg = DEFAULT_BG

    def copy_style(self, other):
        self.attr = other.attr
        self.fg = other.fg
        self.bg = other.bg

    def same_style(self, other):
        return (self.attr == other.attr and self.fg == other.fg
                and self.bg == other.bg)

    def __repr__(self):
        return "Cell(%r, %d, %d, %d)" % (self.ch, self.attr, self.fg, self.bg)


# --- DEC special graphics ---------------------------------------------
# ESC(0 switches G0 to this set. nctest draws its frame with it, so the
# mapping is load-bearing rather than decorative. Keys are the ASCII bytes
# the guest sends; values are the Unicode the box actually needs.
DEC_GRAPHICS = {
    "_": " ",
    "`": "◆",  # diamond
    "a": "▒",  # checkerboard
    "b": "␉",  # HT
    "c": "␌",  # FF
    "d": "␍",  # CR
    "e": "␊",  # LF
    "f": "°",  # degree
    "g": "±",  # plus/minus
    "h": "␤",  # NL
    "i": "␋",  # VT
    "j": "┘",  # lower right corner
    "k": "┐",  # upper right corner
    "l": "┌",  # upper left corner
    "m": "└",  # lower left corner
    "n": "┼",  # crossing lines
    "o": "⎺",  # scan line 1
    "p": "⎻",  # scan line 3
    "q": "─",  # horizontal line
    "r": "⎼",  # scan line 7
    "s": "⎽",  # scan line 9
    "t": "├",  # left tee
    "u": "┤",  # right tee
    "v": "┴",  # bottom tee
    "w": "┬",  # top tee
    "x": "│",  # vertical line
    "y": "≤",
    "z": "≥",
    "{": "π",
    "|": "≠",
    "}": "£",
    "~": "·",  # middle dot
}


class Screen(object):
    """A rows x cols grid of Cells, driven by feed().

    Two buffers, like a real terminal: the primary one, and the alternate
    one that full-screen apps switch to with ESC[?1049h. Keeping them
    separate is what lets nano take over the pane and then give it back
    with the shell scrollback intact -- and it gives hypmon a reliable
    signal for "an app owns the pane now", which is how it decides between
    log mode and screen mode.
    """

    def __init__(self, rows, cols, scrollback=5000):
        self.rows = max(1, rows)
        self.cols = max(1, cols)
        self.scrollback_limit = scrollback

        self.grid = self._new_grid()
        self.alt_grid = None            # set while the alt buffer is active

        self.cx = self.cy = 0
        self.attr = 0
        self.fg = DEFAULT_FG
        self.bg = DEFAULT_BG

        self.top = 0                    # DECSTBM, inclusive, 0-based
        self.bot = self.rows - 1
        self.autowrap = True
        self.insert_mode = False        # IRM. nano turns it off explicitly.
        self.pending_wrap = False       # cursor "past" the last column
        self.cursor_visible = True
        self.saved = None
        self.graphics = False           # G0 is the DEC line-drawing set
        self.shifted = False            # SO seen (G1 selected)

        # Lines that have scrolled off the top of the primary buffer, as
        # (text, attrs) pairs. hypmon's log mode reads these.
        self.scrollback = []
        self.lines_out = 0              # completed lines, ever -- a counter
                                        # the status bar can show honestly

        self.last_ch = None             # for REP; see _dispatch
        self.title = ""
        self.bell_count = 0
        self.unknown = 0                # sequences swallowed; a debug hint
        self.unknown_seqs = []          # ...and what they were, capped.
                                        # Worth keeping: "2 unknown" tells
                                        # you nothing, "ESC[?2004h" tells
                                        # you it was bracketed paste and
                                        # does not matter.

        self._state = "text"
        self._parm = ""
        self._inter = ""
        self._osc = ""
        self._pending = bytearray()     # incomplete UTF-8 across feeds

    # ---- construction helpers ----------------------------------------
    def _new_grid(self):
        return [[Cell() for _ in range(self.cols)] for _ in range(self.rows)]

    @property
    def in_alt(self):
        return self.alt_grid is not None

    def resize(self, rows, cols):
        """Reflow to a new size.

        Content is preserved top-left, which is what every terminal does and
        what apps expect: they redraw after SIGWINCH anyway. The scrolling
        region is reset because a stale region on a resized screen is how you
        get output stuck in a corner.
        """
        rows, cols = max(1, rows), max(1, cols)
        if (rows, cols) == (self.rows, self.cols):
            return
        for name in ("grid", "alt_grid"):
            g = getattr(self, name)
            if g is None:
                continue
            for row in g:
                if cols < len(row):
                    del row[cols:]
                else:
                    row.extend(Cell() for _ in range(cols - len(row)))
            if rows < len(g):
                del g[rows:]
            else:
                g.extend([Cell() for _ in range(cols)]
                         for _ in range(rows - len(g)))
        self.rows, self.cols = rows, cols
        self.top, self.bot = 0, rows - 1
        self.cx = min(self.cx, cols - 1)
        self.cy = min(self.cy, rows - 1)
        self.pending_wrap = False

    # ---- feeding ------------------------------------------------------
    def feed(self, data):
        """Consume bytes. Safe to call with any split point.

        UTF-8 is decoded incrementally: a multi-byte character split across
        two serial reads must not turn into two replacement characters, and
        at 4 Mbps reads land wherever they land.
        """
        if not data:
            return
        if self._pending:
            data = bytes(self._pending) + data
            self._pending = bytearray()

        # Hold back a trailing incomplete UTF-8 sequence (up to 3 bytes).
        cut = len(data)
        for back in range(1, min(4, len(data) + 1)):
            b = data[-back]
            if b < 0x80:
                break
            if b >= 0xC0:                       # a lead byte
                need = (2 if b < 0xE0 else 3 if b < 0xF0 else 4)
                if back < need:
                    cut = len(data) - back
                    self._pending = bytearray(data[cut:])
                break

        for byte in data[:cut]:
            self._byte(byte)

    def _byte(self, b):
        st = self._state
        if st == "text":
            self._text_byte(b)
        elif st == "esc":
            self._esc_byte(b)
        elif st == "csi":
            self._csi_byte(b)
        elif st == "osc":
            self._osc_byte(b)
        elif st == "charset":
            # ESC ( X, ESC ) X -- only the DEC graphics/ASCII distinction
            # matters here.
            self.graphics = (b == 0x30)          # '0'
            self._state = "text"
        elif st == "hash":
            self._state = "text"                 # ESC # 8 (DECALN) etc.

    # ---- ground state -------------------------------------------------
    def _text_byte(self, b):
        if b == 0x1B:
            self._state = "esc"
            self._parm = self._inter = ""
            return
        if b == 0x0D:
            self.cx = 0
            self.pending_wrap = False
            return
        if b in (0x0A, 0x0B, 0x0C):
            self._newline()
            return
        if b == 0x08:
            if self.pending_wrap:
                self.pending_wrap = False
            elif self.cx > 0:
                self.cx -= 1
            return
        if b == 0x09:
            self._tab()
            return
        if b == 0x07:
            self.bell_count += 1
            return
        if b == 0x0E:                            # SO -- select G1
            self.shifted = True
            return
        if b == 0x0F:                            # SI -- back to G0
            self.shifted = False
            return
        if b < 0x20 or b == 0x7F:
            return                               # other C0: ignore

        ch = self._decode(b)
        if ch is None:
            return
        self._put(ch)

    def _decode(self, b):
        """One byte -> one character, honouring the active charset.

        UTF-8 continuation is handled by accumulating in self._pending. Only
        bytes below 0x80 can be graphics-set characters, which is correct:
        the DEC set is a 7-bit replacement.
        """
        if b < 0x80:
            if self.graphics or self.shifted:
                return DEC_GRAPHICS.get(chr(b), chr(b))
            return chr(b)
        # A UTF-8 lead or continuation byte. Accumulate until it decodes.
        self._pending.append(b)
        try:
            text = bytes(self._pending).decode("utf-8")
        except UnicodeDecodeError:
            if len(self._pending) >= 4:
                self._pending = bytearray()
                return "�"
            return None
        self._pending = bytearray()
        return text

    def _put(self, ch):
        if self.pending_wrap and self.autowrap:
            self.cx = 0
            self._newline()
            self.pending_wrap = False
        self.last_ch = ch
        if self.insert_mode:
            row = self.grid[self.cy]
            row.pop(self.cols - 1)
            row.insert(self.cx, Cell())
        cell = self.grid[self.cy][self.cx]
        cell.ch = ch
        cell.attr = self.attr
        cell.fg = self.fg
        cell.bg = self.bg
        if self.cx + 1 >= self.cols:
            # Do not move off-screen: park in the last column and remember
            # that the next character wraps. Writing at the right margin
            # must not scroll until there is something to write.
            self.pending_wrap = True
        else:
            self.cx += 1

    def _tab(self):
        nxt = ((self.cx // 8) + 1) * 8
        self.cx = min(nxt, self.cols - 1)
        self.pending_wrap = False

    def _newline(self):
        self.pending_wrap = False
        if self.cy == self.bot:
            self._scroll_up(1)
        elif self.cy + 1 < self.rows:
            self.cy += 1

    def _scroll_up(self, n):
        for _ in range(n):
            row = self.grid.pop(self.top)
            # Only the primary buffer keeps history. Alt-screen content is
            # transient by definition; saving it would fill the log with
            # half-drawn editor frames.
            if not self.in_alt and self.top == 0:
                self.scrollback.append(self._row_text(row))
                self.lines_out += 1
                if len(self.scrollback) > self.scrollback_limit:
                    del self.scrollback[:len(self.scrollback)
                                        - self.scrollback_limit]
            self.grid.insert(self.bot, [Cell() for _ in range(self.cols)])

    def _scroll_down(self, n):
        for _ in range(n):
            self.grid.pop(self.bot)
            self.grid.insert(self.top, [Cell() for _ in range(self.cols)])

    def _row_text(self, row):
        return "".join(c.ch for c in row).rstrip()

    # ---- escape ------------------------------------------------------
    def _esc_byte(self, b):
        c = chr(b)
        if c == "[":
            self._state = "csi"
            self._parm = self._inter = ""
            return
        if c == "]":
            self._state = "osc"
            self._osc = ""
            return
        if c in "()*+":
            self._state = "charset"
            return
        if c == "#":
            self._state = "hash"
            return
        self._state = "text"
        if c == "7":
            self.save_cursor()
        elif c == "8":
            self.restore_cursor()
        elif c == "M":                      # reverse index
            if self.cy == self.top:
                self._scroll_down(1)
            elif self.cy > 0:
                self.cy -= 1
        elif c in "DE":                     # index / next line
            if c == "E":
                self.cx = 0
            self._newline()
        elif c == "c":
            self.reset()
        elif c in "=>":
            pass                            # keypad mode: nothing to model
        else:
            self._note_unknown("ESC %s" % c)

    def _osc_byte(self, b):
        # OSC ends at BEL or ST (ESC \). Only the title is interesting.
        if b == 0x07 or b == 0x5C and self._osc.endswith("\x1b"):
            body = self._osc[:-1] if self._osc.endswith("\x1b") else self._osc
            if body.startswith(("0;", "2;")):
                self.title = body[2:]
            self._state = "text"
            self._osc = ""
            return
        self._osc += chr(b)
        if len(self._osc) > 512:            # a runaway OSC must not eat RAM
            self._state = "text"
            self._osc = ""

    # ---- CSI ---------------------------------------------------------
    def _csi_byte(self, b):
        c = chr(b)
        if 0x30 <= b <= 0x3F:               # parameter bytes, incl. ? and ;
            self._parm += c
            return
        if 0x20 <= b <= 0x2F:               # intermediate
            self._inter += c
            return
        self._state = "text"
        self._dispatch(c)

    def _parms(self, default=0, count=1):
        out = []
        for p in self._parm.lstrip("?<>=").split(";"):
            out.append(int(p) if p.isdigit() else default)
        while len(out) < count:
            out.append(default)
        return out

    def _dispatch(self, final):
        priv = self._parm.startswith("?")
        p = self._parms()
        n = p[0] if p and p[0] else 1

        if final in "hl":
            if priv:
                self._mode(p, final == "h")
            else:
                self._ansi_mode(p, final == "h")
            return

        if final == "H" or final == "f":            # CUP
            r, col = self._parms(default=1, count=2)
            self.cy = min(max(1, r), self.rows) - 1
            self.cx = min(max(1, col), self.cols) - 1
            self.pending_wrap = False
        elif final == "A":
            self.cy = max(self.top, self.cy - n)
            self.pending_wrap = False
        elif final == "B":
            self.cy = min(self.bot, self.cy + n)
            self.pending_wrap = False
        elif final == "C":
            self.cx = min(self.cols - 1, self.cx + n)
            self.pending_wrap = False
        elif final == "D":
            self.cx = max(0, self.cx - n)
            self.pending_wrap = False
        elif final == "E":
            self.cy = min(self.bot, self.cy + n)
            self.cx = 0
        elif final == "F":
            self.cy = max(self.top, self.cy - n)
            self.cx = 0
        elif final in "G`":                          # CHA / HPA
            self.cx = min(max(1, n), self.cols) - 1
            self.pending_wrap = False
        elif final == "d":                           # VPA
            self.cy = min(max(1, n), self.rows) - 1
            self.pending_wrap = False
        elif final == "J":
            self._erase_display(p[0] if p else 0)
        elif final == "K":
            self._erase_line(p[0] if p else 0)
        elif final == "L":
            self._insert_lines(n)
        elif final == "M":
            self._delete_lines(n)
        elif final == "@":
            self._insert_chars(n)
        elif final == "P":
            self._delete_chars(n)
        elif final == "b":                           # REP
            # Repeat the last graphic character n times. ncurses uses this
            # heavily for horizontal rules -- xterm-256color has `rep`, so
            # box() emits one ─ and then ESC[74b rather than 75 characters.
            # Without it a full-width frame renders as three characters,
            # which is exactly how this gap was found.
            if self.last_ch is not None:
                for _ in range(min(n, self.cols * self.rows)):
                    self._put(self.last_ch)
        elif final == "X":                           # ECH
            for i in range(self.cx, min(self.cols, self.cx + n)):
                self.grid[self.cy][i].blank()
        elif final == "S":
            self._scroll_up(n)
        elif final == "T":
            self._scroll_down(n)
        elif final == "m":
            self._sgr(p)
        elif final == "r":                           # DECSTBM
            top, bot = self._parms(default=0, count=2)
            top = 1 if top == 0 else top
            bot = self.rows if bot == 0 else bot
            if 1 <= top < bot <= self.rows:
                self.top, self.bot = top - 1, bot - 1
                self.cy, self.cx = self.top, 0
        elif final == "s":
            self.save_cursor()
        elif final == "u":
            self.restore_cursor()
        elif final == "t":
            pass                # xterm window ops; the size report among
                                # them is answered by hypmon, upstream
        elif final in "cn":
            pass                # device/cursor reports -- hypmon answers
                                # these itself, upstream of the screen
        else:
            self._note_unknown("CSI %s%s%s" % (self._parm, self._inter, final))

    def _note_unknown(self, seq):
        self.unknown += 1
        if seq not in self.unknown_seqs and len(self.unknown_seqs) < 32:
            self.unknown_seqs.append(seq)

    def _ansi_mode(self, params, on):
        """Non-private mode set/reset.

        Only IRM matters here, and only because nano sends ESC[4l on
        startup to be explicit about replace mode -- which is what this
        emulator does anyway. Handling it turns the last "unknown sequence"
        on a real session into a no-op that is a no-op *on purpose*.
        """
        for m in params:
            if m == 4:
                self.insert_mode = on
            # 20 is LNM (newline vs line feed). The guest's tty driver
            # already does the CR/LF translation, so honouring it here
            # would double it.

    def _mode(self, params, on):
        for m in params:
            if m == 7:
                self.autowrap = on
            elif m == 25:
                self.cursor_visible = on
            elif m in (1047, 1049):
                self._alt_screen(on)
            elif m == 1048:
                self.save_cursor() if on else self.restore_cursor()

    def _alt_screen(self, on):
        if on and not self.in_alt:
            self.save_cursor()
            self.alt_grid = self.grid
            self.grid = self._new_grid()
            self.cx = self.cy = 0
            self.top, self.bot = 0, self.rows - 1
        elif not on and self.in_alt:
            self.grid = self.alt_grid
            self.alt_grid = None
            self.top, self.bot = 0, self.rows - 1
            self.restore_cursor()

    def save_cursor(self):
        self.saved = (self.cx, self.cy, self.attr, self.fg, self.bg,
                      self.graphics, self.shifted)

    def restore_cursor(self):
        if not self.saved:
            self.cx = self.cy = 0
            return
        (self.cx, self.cy, self.attr, self.fg, self.bg,
         self.graphics, self.shifted) = self.saved
        self.cx = min(self.cx, self.cols - 1)
        self.cy = min(self.cy, self.rows - 1)
        self.pending_wrap = False

    # ---- erase / insert ----------------------------------------------
    def _erase_display(self, mode):
        if mode == 0:
            self._erase_line(0)
            for y in range(self.cy + 1, self.rows):
                for c in self.grid[y]:
                    c.blank()
        elif mode == 1:
            self._erase_line(1)
            for y in range(0, self.cy):
                for c in self.grid[y]:
                    c.blank()
        else:
            # ESC[2J and ESC[3J. A full clear is the one case where the
            # departing text is worth keeping: `clear` at the shell would
            # otherwise silently drop a screen of output from the log.
            if not self.in_alt:
                for row in self.grid:
                    text = self._row_text(row)
                    if text:
                        self.scrollback.append(text)
                        self.lines_out += 1
                if len(self.scrollback) > self.scrollback_limit:
                    del self.scrollback[:len(self.scrollback)
                                        - self.scrollback_limit]
            for row in self.grid:
                for c in row:
                    c.blank()
        self.pending_wrap = False

    def _erase_line(self, mode):
        row = self.grid[self.cy]
        if mode == 0:
            rng = range(self.cx, self.cols)
        elif mode == 1:
            rng = range(0, min(self.cx + 1, self.cols))
        else:
            rng = range(0, self.cols)
        for i in rng:
            row[i].blank()
        self.pending_wrap = False

    def _insert_lines(self, n):
        if not self.top <= self.cy <= self.bot:
            return
        for _ in range(min(n, self.bot - self.cy + 1)):
            self.grid.pop(self.bot)
            self.grid.insert(self.cy, [Cell() for _ in range(self.cols)])
        self.cx = 0

    def _delete_lines(self, n):
        if not self.top <= self.cy <= self.bot:
            return
        for _ in range(min(n, self.bot - self.cy + 1)):
            self.grid.pop(self.cy)
            self.grid.insert(self.bot, [Cell() for _ in range(self.cols)])
        self.cx = 0

    def _insert_chars(self, n):
        row = self.grid[self.cy]
        for _ in range(min(n, self.cols - self.cx)):
            row.pop(self.cols - 1)
            row.insert(self.cx, Cell())

    def _delete_chars(self, n):
        row = self.grid[self.cy]
        for _ in range(min(n, self.cols - self.cx)):
            row.pop(self.cx)
            row.append(Cell())

    # ---- SGR ---------------------------------------------------------
    def _sgr(self, params):
        if not params:
            params = [0]
        i = 0
        while i < len(params):
            v = params[i]
            if v == 0:
                self.attr = 0
                self.fg = self.bg = DEFAULT_FG
            elif v == 1:
                self.attr |= BOLD
            elif v == 2:
                self.attr |= DIM
            elif v == 3:
                self.attr |= ITALIC
            elif v == 4:
                self.attr |= UNDERLINE
            elif v == 5:
                self.attr |= BLINK
            elif v == 7:
                self.attr |= REVERSE
            elif v == 22:
                self.attr &= ~(BOLD | DIM)
            elif v == 23:
                self.attr &= ~ITALIC
            elif v == 24:
                self.attr &= ~UNDERLINE
            elif v == 25:
                self.attr &= ~BLINK
            elif v == 27:
                self.attr &= ~REVERSE
            elif 30 <= v <= 37:
                self.fg = v - 30
            elif v == 39:
                self.fg = DEFAULT_FG
            elif 40 <= v <= 47:
                self.bg = v - 40
            elif v == 49:
                self.bg = DEFAULT_BG
            elif 90 <= v <= 97:
                self.fg = v - 90 + 8
            elif 100 <= v <= 107:
                self.bg = v - 100 + 8
            elif v in (38, 48):
                # 256-colour and truecolour. Consume the arguments so they
                # are never mistaken for further attributes -- the classic
                # way a naive SGR parser turns one colour into five.
                if i + 1 < len(params) and params[i + 1] == 5:
                    col = params[i + 2] if i + 2 < len(params) else 0
                    i += 2
                elif i + 1 < len(params) and params[i + 1] == 2:
                    col = params[i + 2] if i + 2 < len(params) else 0
                    i += 4
                else:
                    i += 1
                    col = 0
                if v == 38:
                    self.fg = col
                else:
                    self.bg = col
            i += 1

    # ---- output ------------------------------------------------------
    def reset(self):
        self.grid = self._new_grid()
        self.alt_grid = None
        self.cx = self.cy = 0
        self.attr = 0
        self.fg = self.bg = DEFAULT_FG
        self.top, self.bot = 0, self.rows - 1
        self.autowrap = True
        self.insert_mode = False
        self.pending_wrap = False
        self.cursor_visible = True
        self.graphics = self.shifted = False
        self.saved = None

    def line(self, y):
        """Row y as text, trailing blanks stripped."""
        return self._row_text(self.grid[y])

    def text(self):
        """The whole visible screen as text. For tests and for --dump."""
        return "\n".join(self.line(y) for y in range(self.rows))

    def all_lines(self):
        """Scrollback followed by the primary buffer's visible rows.

        This is what log mode shows: the transcript, never the alt buffer's
        contents -- you want the shell history behind nano, not nano's own
        redraw frames.

        Note it reads the PRIMARY grid even while the alt buffer is active,
        because scrollback only holds rows that have scrolled off the top.
        Everything still on screen when nano started -- usually the whole
        session so far on a fresh boot -- lives in the grid, and reading
        self.grid here would silently drop it for as long as an editor was
        open. While alt is active the primary grid is parked in alt_grid.
        """
        return [t for t, _ in self.all_lines_with_ids()]

    def all_lines_with_ids(self):
        """all_lines(), each line paired with a stable transcript index.

        The index is what lets a caller attach a timestamp to a line and
        have it stay attached. It is stable under scrolling: a row visible
        at grid position r when lines_out was L has index L + r, and after
        a scroll it sits at r-1 with lines_out L+1 -- same index. When it
        finally falls off into the scrollback the arithmetic still lands on
        the same number.

        Stability is the whole point. Indexing by grid row would re-stamp
        every line on every scroll, and indexing by scrollback position
        would leave the visible tail -- which is the part you are actually
        looking at -- with no timestamp at all.
        """
        evicted = self.lines_out - len(self.scrollback)
        out = [(text, evicted + i) for i, text in enumerate(self.scrollback)]
        primary = self.alt_grid if self.in_alt else self.grid
        rows = [self._row_text(r) for r in primary]
        while rows and not rows[-1]:
            rows.pop()
        out.extend((text, self.lines_out + r) for r, text in enumerate(rows))
        return out
