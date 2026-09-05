/*
 * Smallest thing that proves the whole ncurses path works on the guest:
 * libncurses linked into a bFLT binary, a terminfo entry findable at runtime,
 * and cursor addressing over the emulated 16550.
 *
 * The serial console reports no window size (TIOCGWINSZ returns 0x0), so
 * ncurses falls back to the terminfo geometry for TERM. If LINES/COLUMNS come
 * out as 0 here, fix it from the shell with `resize` or `stty rows 24 cols 80`
 * rather than in this program -- it is a terminal problem, not an app problem.
 */

#include <ncurses.h>

int main(void)
{
    int ch;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    box(stdscr, 0, 0);
    mvprintw(2, 4, "ncurses on a U-mode Linux guest, ESP32-P4 M-mode monitor");
    mvprintw(4, 4, "terminal : %s", termname());
    mvprintw(5, 4, "geometry : %d rows x %d cols", LINES, COLS);
    mvprintw(6, 4, "colors   : %d", has_colors() ? COLORS : 0);
    mvprintw(8, 4, "press a key to echo it, q to quit");
    refresh();

    while ((ch = getch()) != (int)0x71) {
        mvprintw(10, 4, "key: %-5d %-12s", ch, keyname(ch));
        clrtoeol();
        box(stdscr, 0, 0);
        refresh();
    }

    endwin();
    return 0;
}
