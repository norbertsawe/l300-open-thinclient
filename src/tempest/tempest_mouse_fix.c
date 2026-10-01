/* RAM-only Tempest mouse compatibility shim.
 *
 * Physical L300 capture showed that the vendor length-10 decoder reads the
 * mouse payload one byte too early.  The original decoder is still executed,
 * but its mouse input_event() arguments are corrected before reaching evdev.
 *
 * Original vendor length-10 interpretation:
 *   buttons = b[1] & 7
 *   X       = s12(b[2] | ((b[3] & 15) << 8))
 *   Y       = s12((b[3] >> 4) | (b[4] << 4))
 *   wheel   = s8(b[5])
 *
 * Physical L300 report alignment:
 *   buttons = b[2] & 7
 *   X       = s12(b[3] | ((b[4] & 15) << 8))
 *   Y       = s12((b[4] >> 4) | (b[5] << 4))
 *   wheel   = s8(b[6])
 *
 * Only receive-length 10 mouse reports are modified.
 * Other decoder branches and all non-mouse events pass through unchanged.
 *
 * GPL-2.0-only
 */

extern void store_mouse_event(void *port, unsigned char *data);
extern void input_event(void *dev, unsigned type, unsigned code, int value);
extern int printk(const char *format, ...);
extern unsigned G_port0[];

struct observation {
    int x, y, wheel;
    unsigned buttons, seen;

    int fix_active;
    int fix_x, fix_y, fix_wheel;
    unsigned fix_buttons;
};

static struct observation *active;
static unsigned report_count;

static int sign12(unsigned v)
{
    v &= 0x0fff;
    return (v & 0x0800) ? (int)(v | 0xfffff000U) : (int)v;
}

static int sign8(unsigned v)
{
    v &= 0xff;
    return (v & 0x80) ? (int)(v | 0xffffff00U) : (int)v;
}

void observe_input_event(void *dev, unsigned type, unsigned code, int value)
{
    if (active) {
        /*
         * Correct only mouse events produced while decoding a captured
         * length-10 Tempest report.
         */
        if (active->fix_active) {
            if (type == 2 && code == 0)
                value = active->fix_x;
            else if (type == 2 && code == 1)
                value = active->fix_y;
            else if (type == 2 && code == 8)
                value = active->fix_wheel;
            else if (type == 1 && code >= 272 && code <= 274)
                value = !!(active->fix_buttons & (1U << (code - 272)));
        }

        if (type == 2 && code == 0) {
            active->x = value;
            active->seen |= 1;
        }
        if (type == 2 && code == 1) {
            active->y = value;
            active->seen |= 2;
        }
        if (type == 2 && code == 8) {
            active->wheel = value;
            active->seen |= 4;
        }
        if (type == 1 && code >= 272 && code <= 274) {
            unsigned bit = 1U << (code - 272);
            if (value)
                active->buttons |= bit;
            else
                active->buttons &= ~bit;
            active->seen |= bit << 3;
        }
    }

    input_event(dev, type, code, value);
}

void observe_store_mouse_event(void *port, unsigned char *data)
{
    unsigned *p = port;
    struct observation frame = {0,0,0,0,0,0,0,0,0,0};
    struct observation *previous = active;
    unsigned char bytes[16];
    char hex[33];
    static const char digits[] = "0123456789abcdef";
    unsigned i, n = p[26], length = n, race = 0;

    if (n > 16)
        n = 16;

    for (i = 0; i < n; i++)
        bytes[i] = data[i];

    /*
     * Real-hardware capture established that the length-10 Tempest mouse
     * payload is displaced by one byte relative to the vendor decoder.
     */
    if (length == 10) {
        unsigned xv, yv;

        frame.fix_active = 1;
        frame.fix_buttons = bytes[2] & 7;

        xv = (unsigned)bytes[3] |
             (((unsigned)bytes[4] & 15U) << 8);

        yv = ((unsigned)bytes[4] >> 4) |
             ((unsigned)bytes[5] << 4);

        frame.fix_x = sign12(xv);
        frame.fix_y = sign12(yv);
        frame.fix_wheel = sign8(bytes[6]);
    }

    active = &frame;
    store_mouse_event(port, data);
    active = previous;

    /* Detect a changed shared receive buffer without altering its contents. */
    for (i = 0; i < n; i++) {
        if (bytes[i] != data[i])
            race = 1;

        hex[2*i] = digits[bytes[i] >> 4];
        hex[2*i+1] = digits[bytes[i] & 15];
    }

    hex[2*n] = 0;

    if (report_count < 80) {
        report_count++;

        printk("<5>TMPFIX n=%u port=%u len=%u raw=%s "
               "x=%d y=%d w=%d b=%x seen=%02x race=%u\n",
               report_count,
               p == G_port0 ? 0 : 1,
               length,
               hex,
               frame.x,
               frame.y,
               frame.wheel,
               frame.buttons,
               frame.seen,
               race);

        if (report_count == 80)
            printk("<5>TMPFIX LIMIT 80 reports\n");
    }
}
