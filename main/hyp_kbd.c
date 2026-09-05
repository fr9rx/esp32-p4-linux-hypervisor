/* A USB keyboard, wired into the guest's 16550 receive path. See hyp_kbd.h. */

#include <string.h>

#include "hyp_kbd.h"

#include "esp_rom_sys.h"

#include "usbh_core.h"
#include "usbh_hid.h"

/* ---------------------------------------------------------------------------
 * The ring that crosses cores
 *
 * Core 0's poll thread produces, core 1's trap handler consumes. Single
 * producer, single consumer, so the indices need no lock -- only a fence, for
 * the same reason as in hyp_console.c: internal SRAM is coherent between the
 * two L1 caches on this part, but that says nothing about store ordering.
 * --------------------------------------------------------------------------- */

#define KBD_RING_SIZE 256u              /* power of two; ~30 keystrokes of slack */

static struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t overruns;
    volatile uint32_t devices;
    volatile uint32_t reports;
    uint8_t  ring[KBD_RING_SIZE];
} kbd;

static void kbd_push(uint8_t c)
{
    uint32_t head = kbd.head;
    uint32_t next = (head + 1u) & (KBD_RING_SIZE - 1u);
    if (next == kbd.tail) {
        kbd.overruns++;
        return;
    }
    kbd.ring[head] = c;
    asm volatile ("fence w, w" ::: "memory");
    kbd.head = next;
}

static void kbd_push_str(const char *s)
{
    while (*s != '\0') {
        kbd_push((uint8_t)*s++);
    }
}

bool hyp_kbd_pop(uint8_t *out)
{
    uint32_t tail = kbd.tail;
    if (tail == kbd.head) {
        return false;
    }
    asm volatile ("fence r, r" ::: "memory");
    *out = kbd.ring[tail];
    kbd.tail = (tail + 1u) & (KBD_RING_SIZE - 1u);
    return true;
}

uint32_t hyp_kbd_devices(void)  { return kbd.devices; }
uint32_t hyp_kbd_reports(void)  { return kbd.reports; }
uint32_t hyp_kbd_overruns(void) { return kbd.overruns; }


/* ---------------------------------------------------------------------------
 * HID boot protocol -> ASCII
 *
 * The boot keyboard report is fixed by the HID spec at 8 bytes: a modifier
 * bitmap, one reserved byte, then up to six simultaneously-held usage IDs. It
 * is a *state* report, not an event stream, so which keys are newly down has to
 * be worked out by diffing against the previous report -- which is also what
 * makes auto-repeat this driver's job rather than the keyboard's.
 *
 * Usage IDs are from the HID Keyboard/Keypad usage page (HUT 1.12 section 10);
 * the two tables below are that page's printable range, unshifted and shifted.
 * A zero means "nothing to send as a character" -- those usages are handled
 * separately if they map to an escape sequence, and dropped otherwise.
 * --------------------------------------------------------------------------- */

#define MOD_CTRL   0x11u                /* bit0 LCtrl  | bit4 RCtrl  */
#define MOD_SHIFT  0x22u                /* bit1 LShift | bit5 RShift */
#define MOD_ALT    0x44u                /* bit2 LAlt   | bit6 RAlt   */

#define USAGE_MAX  0x64u

static const char usage_plain[USAGE_MAX] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* 0x04 */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
    /* 0x11 */ 'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1e */ '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 */ '\r',                    /* Enter: the tty layer turns CR into NL */
    /* 0x29 */ 0x1b,                    /* Escape */
    /* 0x2a */ 0x7f,                    /* Backspace: DEL, which is what stty
                                         * erase expects on a Linux console */
    /* 0x2b */ '\t',
    /* 0x2c */ ' ',
    /* 0x2d */ '-','=','[',']','\\',
    /* 0x32 */ 0,                       /* non-US # -- no sane mapping */
    /* 0x33 */ ';','\'','`',',','.','/',
    /* 0x39 */ 0,                       /* CapsLock: not tracked */
    /* 0x3a */ 0,0,0,0,0,0,0,0,0,0,0,0, /* F1..F12, handled as escapes */
    /* 0x46 */ 0,0,0,                   /* PrintScreen, ScrollLock, Pause */
    /* 0x49 */ 0,                       /* Insert */
    /* 0x4a */ 0,0,0,0,0,               /* Home PageUp Delete End PageDown */
    /* 0x4f */ 0,0,0,0,                 /* Right Left Down Up */
    /* 0x53 */ 0,                       /* NumLock */
    /* 0x54 */ '/','*','-','+',
    /* 0x58 */ '\r',                    /* keypad Enter */
    /* 0x59 */ '1','2','3','4','5','6','7','8','9','0','.',
};

static const char usage_shift[USAGE_MAX] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* 0x04 */ 'A','B','C','D','E','F','G','H','I','J','K','L','M',
    /* 0x11 */ 'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    /* 0x1e */ '!','@','#','$','%','^','&','*','(',')',
    /* 0x28 */ '\r',
    /* 0x29 */ 0x1b,
    /* 0x2a */ 0x7f,
    /* 0x2b */ '\t',
    /* 0x2c */ ' ',
    /* 0x2d */ '_','+','{','}','|',
    /* 0x32 */ 0,
    /* 0x33 */ ':','"','~','<','>','?',
    /* 0x39 */ 0,
    /* 0x3a */ 0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x46 */ 0,0,0,
    /* 0x49 */ 0,
    /* 0x4a */ 0,0,0,0,0,
    /* 0x4f */ 0,0,0,0,
    /* 0x53 */ 0,
    /* 0x54 */ '/','*','-','+',
    /* 0x58 */ '\r',
    /* 0x59 */ '1','2','3','4','5','6','7','8','9','0','.',
};

/* Keys that are sequences rather than characters. The arrow and editing keys
 * are the xterm/vt100 forms that ncurses' vt100 entry expects; F1..F4 are the
 * SS3 forms and F5..F12 the CSI ~ forms, which is what a real xterm sends. */
static const char *usage_escape(uint8_t usage)
{
    switch (usage) {
    case 0x3a: return "\x1bOP";      /* F1  */
    case 0x3b: return "\x1bOQ";      /* F2  */
    case 0x3c: return "\x1bOR";      /* F3  */
    case 0x3d: return "\x1bOS";      /* F4  */
    case 0x3e: return "\x1b[15~";    /* F5  */
    case 0x3f: return "\x1b[17~";    /* F6  */
    case 0x40: return "\x1b[18~";    /* F7  */
    case 0x41: return "\x1b[19~";    /* F8  */
    case 0x42: return "\x1b[20~";    /* F9  */
    case 0x43: return "\x1b[21~";    /* F10 */
    case 0x44: return "\x1b[23~";    /* F11 */
    case 0x45: return "\x1b[24~";    /* F12 */
    case 0x49: return "\x1b[2~";     /* Insert   */
    case 0x4a: return "\x1b[1~";     /* Home     */
    case 0x4b: return "\x1b[5~";     /* PageUp   */
    case 0x4c: return "\x1b[3~";     /* Delete   */
    case 0x4d: return "\x1b[4~";     /* End      */
    case 0x4e: return "\x1b[6~";     /* PageDown */
    case 0x4f: return "\x1b[C";      /* Right */
    case 0x50: return "\x1b[D";      /* Left  */
    case 0x51: return "\x1b[B";      /* Down  */
    case 0x52: return "\x1b[A";      /* Up    */
    default:   return NULL;
    }
}

static void emit_usage(uint8_t usage, uint8_t mods)
{
    const char *seq = usage_escape(usage);
    if (seq != NULL) {
        kbd_push_str(seq);
        return;
    }
    if (usage >= USAGE_MAX) {
        return;
    }

    char c = ((mods & MOD_SHIFT) != 0u) ? usage_shift[usage] : usage_plain[usage];
    if (c == 0) {
        return;
    }

    /* Control characters come from the letter, not from the shifted form, so
     * that Ctrl-C is 0x03 whether or not shift is held. Ctrl-Space is NUL and
     * Ctrl-[ is Escape, both of which real terminals do send. */
    if ((mods & MOD_CTRL) != 0u) {
        char base = usage_plain[usage];
        if (base >= 'a' && base <= 'z') {
            kbd_push((uint8_t)(base - 'a' + 1));
            return;
        }
        if (base == ' ')  { kbd_push(0x00u); return; }
        if (base == '[')  { kbd_push(0x1bu); return; }
        if (base == '\\') { kbd_push(0x1cu); return; }
        if (base == ']')  { kbd_push(0x1du); return; }
        if (base == '-')  { kbd_push(0x1fu); return; }
        /* Anything else with ctrl held: send the character unmodified rather
         * than swallow it. */
    }

    /* Alt is the classic ESC prefix, which is what readline and vi expect. */
    if ((mods & MOD_ALT) != 0u) {
        kbd_push(0x1bu);
    }
    kbd_push((uint8_t)c);
}


/* ---------------------------------------------------------------------------
 * The poll thread
 *
 * Two things here were got wrong first, both of which presented identically --
 * "task_wdt: CPU 0: hyp_kbd" every five seconds forever, core 0's idle task
 * never running again -- and only one of them was the obvious one.
 *
 * 1. THE REPORT BUFFER MUST BE 64-BYTE ALIGNED, not 4. usbh_submit_urb() opens
 *    with USB_ASSERT_MSG on urb->transfer_buffer % CONFIG_USB_ALIGN_SIZE, and
 *    on this platform usb_config.h defines CONFIG_USB_DCACHE_ENABLE and sets
 *    CONFIG_USB_ALIGN_SIZE to CONFIG_CACHE_L1_CACHE_LINE_SIZE, i.e. 64 -- the
 *    buffer is a DMA target whose cache lines the driver invalidates, so it
 *    must not share a line with anything else. USB_ASSERT_MSG's failure path is
 *    a while(1), so a misaligned buffer does not return an error code, it hangs
 *    the calling task inside the submit. The board does say so
 *    ("transfer_buffer is not aligned 64") but the message interleaves with the
 *    guest's console output and is easy to scroll past.
 *
 * 2. The transfer has to be asynchronous. Submitting with a timeout and looping
 *    on the return value is the obvious reading of the API and is wrong:
 *    usbh_submit_urb() only blocks on its completion semaphore once it has got
 *    as far as arming a channel, and every early return above that point comes
 *    back instantly. A loop that treats those as "try again" spins at full
 *    speed. So: submit with timeout 0, wait on our own semaphore, which the
 *    completion callback gives.
 *
 * The urb is deliberately left pending across a wait that times out. Killing
 * and resubmitting each time would race -- a report completing just as the kill
 * lands leaves a stale semaphore count and the next wait returns immediately
 * with a report that has already been handled. A timeout here simply means "no
 * key yet", so the right response is to tick the auto-repeat and wait again.
 * --------------------------------------------------------------------------- */

#define REPEAT_DELAY_MS   400u          /* before a held key starts repeating */
#define REPEAT_PERIOD_MS   35u          /* between repeats once it has */
#define REPORT_WAIT_MS     35u          /* wait granularity; also the repeat clock */
#define RETRY_SLEEP_MS     20u          /* after a submit error, before retrying */

/* Big enough for any boot report with room to spare, a whole number of cache
 * lines long, and aligned to one. Static because it has to outlive each urb. */
#define KBD_REPORT_SIZE   USB_ALIGN_UP(64, CONFIG_USB_ALIGN_SIZE)

static void hyp_kbd_thread(void *arg);

/* Called from the USB interrupt on completion. Only ever gives the semaphore --
 * decoding happens in the thread. */
static void hyp_kbd_urb_done(void *arg, int nbytes)
{
    (void)nbytes;
    usb_osal_sem_give((usb_osal_sem_t)arg);
}

/* CherryUSB's HID class driver calls this once per HID interface it binds. The
 * default implementation is __WEAK, so defining it here replaces it -- which is
 * the documented way to own the transfer loop. */
void usbh_hid_run(struct usbh_hid *hid_class)
{
    static uint8_t minor_seen;

    /* Boot protocol only. A report-protocol keyboard sends whatever its report
     * descriptor says, which would mean parsing that descriptor; SET_PROTOCOL 0
     * asks for the fixed 8-byte layout instead, which every keyboard supports
     * because the PC BIOS has always depended on it.
     *
     * A real keyboard turns up as three HID interfaces -- observed on a
     * Microsoft 045e:0745: protocol 1 the keyboard on ep 0x81 with 8-byte
     * reports, protocol 2 a mouse on 0x82, protocol 0 the consumer/media keys
     * on 0x83. Only the first is ours. */
    if (hid_class->protocol != HID_PROTOCOL_KEYBOARD) {
        esp_rom_printf("I: kbd: ignoring HID interface %u (protocol %u)\n",
                       (unsigned)hid_class->intf, (unsigned)hid_class->protocol);
        return;
    }
    if (hid_class->intin == NULL) {
        esp_rom_printf("W: kbd: keyboard has no interrupt IN endpoint\n");
        return;
    }

    hid_class->minor = minor_seen++;
    kbd.devices++;

    if (usb_osal_thread_create("hyp_kbd", 3072, CONFIG_USBHOST_PSC_PRIO + 1,
                               hyp_kbd_thread, hid_class) == NULL) {
        esp_rom_printf("E: kbd: could not create the poll thread\n");
    }
}

/* Decode one boot report.
 *
 * Split out of the poll thread on purpose: it is the half of the input path
 * that cannot be exercised without a human pressing a key, so being able to
 * drive it from a synthetic report is the difference between "believed to work"
 * and "seen to work". hyp_kbd_selftest() does exactly that.
 *
 * The press-tracking state is static rather than local for the same reason --
 * both callers share one keyboard state machine. */
static uint8_t kbd_prev[6];
static uint8_t kbd_held;                /* usage being auto-repeated, 0 for none */
static uint8_t kbd_held_mods;
static uint32_t kbd_held_ms;

static void kbd_handle_report(const uint8_t *keys, uint8_t mods)
{
    /* The report is a state, not an event, so only usages absent from the
     * previous one are new presses. Without this diff, holding a key would type
     * it once per report rather than once per press. */
    for (int i = 0; i < 6; i++) {
        uint8_t usage = keys[i];
        if (usage == 0u || usage == 0x01u) {
            continue;                   /* 0x01 is ErrorRollOver */
        }
        bool was_down = false;
        for (int j = 0; j < 6; j++) {
            if (kbd_prev[j] == usage) { was_down = true; break; }
        }
        if (!was_down) {
            emit_usage(usage, mods);
            kbd_held = usage;           /* last new key wins the repeat */
            kbd_held_mods = mods;
            kbd_held_ms = 0u;
        }
    }

    /* Repeat only while that exact key is still down. */
    bool still_held = false;
    for (int i = 0; i < 6; i++) {
        if (kbd_held != 0u && keys[i] == kbd_held) { still_held = true; break; }
    }
    if (!still_held) {
        kbd_held = 0u;
        kbd_held_ms = 0u;
    }

    memcpy(kbd_prev, keys, sizeof(kbd_prev));
}

/* Type a fixed string as though it had been pressed on the keyboard.
 *
 * Drives kbd_handle_report() with synthesised press/release pairs, so it
 * exercises everything from the HID usage tables down: the press diff, the
 * usage-to-ASCII mapping, the ring, and the trap handler's drain into the
 * guest's 16550. The only link it does not cover is the USB transfer itself.
 *
 * Triggered by Ctrl-_ on the serial port. Worth having permanently: without it
 * the input path can only be tested by a human at the keyboard, and "the
 * keyboard does nothing" is otherwise indistinguishable from a dozen causes. */
void hyp_kbd_selftest(void)
{
    static const uint8_t usages[] = {
        0x0e, 0x05, 0x07, 0x2d, 0x12, 0x0e, 0x28,   /* k b d - o k Enter */
    };
    uint8_t frame[6] = { 0 };

    esp_rom_printf("I: kbd: self-test, typing kbd-ok\n");
    for (unsigned i = 0u; i < sizeof(usages); i++) {
        frame[0] = usages[i];
        kbd_handle_report(frame, 0u);           /* press   */
        frame[0] = 0u;
        kbd_handle_report(frame, 0u);           /* release */
    }
}

static void hyp_kbd_thread(void *arg)
{
    struct usbh_hid *hid = (struct usbh_hid *)arg;
    static uint8_t report[KBD_REPORT_SIZE]
        __attribute__((aligned(CONFIG_USB_ALIGN_SIZE)));

    bool pending = false;

    usb_osal_sem_t done = usb_osal_sem_create(0);
    if (done == NULL) {
        esp_rom_printf("E: kbd: no semaphore for the poll thread\n");
        usb_osal_thread_delete(NULL);
        return;
    }

    (void)usbh_hid_set_protocol(hid, 0);    /* 0 = boot protocol */

    uint16_t mps = hid->intin->wMaxPacketSize;
    if (mps > KBD_REPORT_SIZE) {
        mps = KBD_REPORT_SIZE;
    }
    esp_rom_printf("I: kbd: keyboard on interface %u, %u-byte reports, "
                   "buffer @0x%08x\n",
                   (unsigned)hid->intf, (unsigned)mps,
                   (unsigned)(uintptr_t)report);

    for (;;) {
        if (!pending) {
            usbh_int_urb_fill(&hid->intin_urb, hid->hport, hid->intin, report,
                              mps, 0, hyp_kbd_urb_done, done);
            int ret = usbh_submit_urb(&hid->intin_urb);
            if (ret == -USB_ERR_SHUTDOWN || ret == -USB_ERR_NOTCONN) {
                break;                  /* unplugged */
            }
            if (ret < 0) {
                /* Sleep before retrying. Without this, any instant-return error
                 * is an unbounded spin -- see the header comment. */
                usb_osal_msleep(RETRY_SLEEP_MS);
                continue;
            }
            pending = true;
        }

        if (usb_osal_sem_take(done, REPORT_WAIT_MS) < 0) {
            /* No key yet, and the urb is still armed. Do not touch it. */
            if (kbd_held != 0u) {
                kbd_held_ms += REPORT_WAIT_MS;
                if (kbd_held_ms >= REPEAT_DELAY_MS) {
                    emit_usage(kbd_held, kbd_held_mods);
                    kbd_held_ms -= REPEAT_PERIOD_MS;
                }
            }
            continue;
        }
        pending = false;

        if (hid->intin_urb.errorcode == -USB_ERR_SHUTDOWN ||
            hid->intin_urb.errorcode == -USB_ERR_NOTCONN) {
            break;
        }
        if (hid->intin_urb.errorcode < 0 || hid->intin_urb.actual_length < 3) {
            usb_osal_msleep(RETRY_SLEEP_MS);
            continue;                   /* stall, babble, runt: let it recover */
        }

        kbd_handle_report(&report[2], report[0]);
        kbd.reports++;
    }

    if (pending) {
        usbh_kill_urb(&hid->intin_urb);
    }
    esp_rom_printf("I: kbd: keyboard gone\n");
    usb_osal_sem_delete(done);
    usb_osal_thread_delete(NULL);
}

bool hyp_kbd_init(void)
{
    int ret = usbh_initialize(0, ESP_USB_HS0_BASE);
    if (ret < 0) {
        esp_rom_printf("E: kbd: usbh_initialize: %d\n", ret);
        return false;
    }
    esp_rom_printf("I: kbd: USB host up on HS0; plug in a keyboard\n");
    return true;
}
