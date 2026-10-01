/* SPDX-License-Identifier: Apache-2.0
 * Small Linux fbdev/evdev frontend for FreeRDP's 2011 interface.
 * Target: SPEAr600 / ARM926EJ-S, Linux 2.6.19.2, ELDK 4.1 OABI.
 */
#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <openssl/ssl.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#include <openssl/rc4.h>
#include <openssl/rand.h>
#include <freerdp/freerdp.h>
#include <freerdp/utils/unicode.h>
#include "gdi.h"
#include "status-font.h"

#define L300_MAX_INPUT 16
#define MAX_WIDTH 1024
#define MAX_HEIGHT 768
#define MAX_CURSOR 96
#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define BIT_TEST(a,b) (((a)[(b)/BITS_PER_LONG] >> ((b)%BITS_PER_LONG)) & 1)

typedef struct {
    int width, height, hotx, hoty;
    uint32 pixels[MAX_CURSOR * MAX_CURSOR]; /* ARGB8888 */
} Cursor;

typedef struct {
    int fd, width, height, bytes, pitch, xoffset, yoffset, headless;
    size_t length;
    unsigned char *memory;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint32 lut[65536];
    int input[L300_MAX_INPUT], ninput;
    char input_path[L300_MAX_INPUT][64];
    unsigned diag_counts[L300_MAX_INPUT][4], diag_lines, diag_sequence;
    int mouse_diag, diag_fd;
    int mouse_x, mouse_y, hidden, buttons, toggles;
    unsigned char keys[256];
    Cursor arrow, *cursor;
    char cert_pin[65];
    int error, timeout, local_exit;
} Frontend;

/* Optional observation only: no input scaling, filtering, grabs or RDP changes.
 * Serial output is nonblocking and best-effort; stderr retains complete lines.
 * Keep diagnostics bounded and never record ordinary keyboard/password keys. */
static void mouse_diag_line(Frontend *f, const char *format, ...)
{
    char line[256]; int length; va_list args;
    if (!f->mouse_diag || f->diag_lines >= 600) return;
    if (f->diag_lines == 599) {
        strcpy(line,"MOUSE-DIAG LIMIT reached; restart client for a fresh capture\n");
    } else {
        va_start(args,format);
        vsnprintf(line,sizeof(line)-1,format,args);
        va_end(args);
        line[sizeof(line)-2]=0;
        strcat(line,"\n");
    }
    length=strlen(line);
    fwrite(line,1,length,stderr);
    if (f->diag_fd >= 0) (void)write(f->diag_fd,line,length);
    f->diag_lines++;
}

static void diagnose_input(Frontend *f, int slot, int fd, const char *name,
                           unsigned long *types)
{
    unsigned long rel[(REL_MAX+BITS_PER_LONG)/BITS_PER_LONG];
    unsigned long keys[(KEY_MAX+BITS_PER_LONG)/BITS_PER_LONG];
    struct stat st; struct input_id id; char phys[128];
    int rel_ok,key_ok,i;
    if (!f->mouse_diag) return;
    memset(rel,0,sizeof(rel)); memset(keys,0,sizeof(keys));
    memset(&st,0,sizeof(st)); memset(&id,0,sizeof(id)); memset(phys,0,sizeof(phys));
    fstat(fd,&st); ioctl(fd,EVIOCGID,&id); ioctl(fd,EVIOCGPHYS(sizeof(phys)-1),phys);
    rel_ok=ioctl(fd,EVIOCGBIT(EV_REL,sizeof(rel)),rel)>=0;
    key_ok=ioctl(fd,EVIOCGKEY(sizeof(keys)),keys)>=0;
    mouse_diag_line(f,"MOUSE-DIAG DEVICE slot=%d path=%s rdev=0x%lx name=%.80s",
                    slot,f->input_path[slot],(unsigned long)st.st_rdev,name);
    mouse_diag_line(f,"MOUSE-DIAG CAPS slot=%d EV_KEY=%d EV_REL=%d rel_ok=%d rel_mask=0x%lx X=%d Y=%d WHEEL=%d",
                    slot,(int)BIT_TEST(types,EV_KEY),(int)BIT_TEST(types,EV_REL),
                    rel_ok,rel[0],(int)BIT_TEST(rel,REL_X),(int)BIT_TEST(rel,REL_Y),
                    (int)BIT_TEST(rel,REL_WHEEL));
    mouse_diag_line(f,"MOUSE-DIAG ID slot=%d bus=%04x vendor=%04x product=%04x version=%04x phys=%.100s",
                    slot,id.bustype,id.vendor,id.product,id.version,phys);
    mouse_diag_line(f,"MOUSE-DIAG INITIAL slot=%d keys_ok=%d left=%d right=%d middle=%d",
                    slot,key_ok,(int)BIT_TEST(keys,BTN_LEFT),(int)BIT_TEST(keys,BTN_RIGHT),
                    (int)BIT_TEST(keys,BTN_MIDDLE));
    for(i=0;i<slot;i++) {
        struct stat other;
        if(fstat(f->input[i],&other)==0 && other.st_rdev==st.st_rdev)
            mouse_diag_line(f,"MOUSE-DIAG ALIAS slot=%d same_device_as=%d (observation only)",slot,i);
    }
}

static int diagnose_event(Frontend *f, int slot, const struct input_event *ev)
{
    int group; unsigned limit;
    if(!f->mouse_diag) return 0;
    if(ev->type==EV_REL) { group=ev->code==REL_WHEEL ? 1 : 0; limit=group ? 24 : 80; }
    else if(ev->type==EV_KEY && ev->code>=BTN_MOUSE && ev->code<=BTN_TASK) { group=2; limit=24; }
    else if(ev->type==EV_SYN) { group=3; limit=40; }
    else return 0;
    f->diag_sequence++;
    if(f->diag_counts[slot][group]++ >= limit) return 0;
    mouse_diag_line(f,"MOUSE-DIAG RAW seq=%u slot=%d path=%s time=%ld.%06ld type=%u code=%u value=%d hex=0x%08x",
                    f->diag_sequence,slot,f->input_path[slot],(long)ev->time.tv_sec,
                    (long)ev->time.tv_usec,ev->type,ev->code,ev->value,(unsigned)ev->value);
    if(f->diag_counts[slot][group]==limit)
        mouse_diag_line(f,"MOUSE-DIAG CATEGORY-LIMIT slot=%d group=%d last_raw_seq=%u",slot,group,f->diag_sequence);
    return 1;
}

static void diagnose_send(Frontend *f, int slot, int flags)
{
    int rotation=flags & PTRFLAGS_WHEEL_ROTATION_MASK;
    if(rotation & PTRFLAGS_WHEEL_NEGATIVE) rotation-=512;
    mouse_diag_line(f,"MOUSE-DIAG SEND seq=%u slot=%d flags=0x%04x x=%d y=%d wheel=%d buttons=0x%04x",
                    f->diag_sequence,slot,flags,f->mouse_x,f->mouse_y,
                    flags & PTRFLAGS_WHEEL ? rotation : 0,f->buttons);
}

static volatile sig_atomic_t running = 1;
static Frontend *front(rdpInst *inst) { return (Frontend *)inst->param1; }
static void stop_signal(int sig) { (void)sig; running = 0; }
static void connect_timeout(int sig)
{
    static const char message[] = "Connection or incomplete RDP packet timed out.\n";
    (void)sig;
    write(2, message, sizeof(message)-1);
    _exit(124); /* Kernel releases input grabs; shell launcher resumes fbterm. */
}

static uint32 fb_colour(Frontend *f, unsigned r, unsigned g, unsigned b)
{
    /* The L300 VGA hardware reverses red/blue relative to the
       channel offsets reported by its framebuffer driver. */
    if (!f->headless) {
        return ((b >> (8-f->var.red.length)) << f->var.red.offset) |
               ((g >> (8-f->var.green.length)) << f->var.green.offset) |
               ((r >> (8-f->var.blue.length)) << f->var.blue.offset);
    }

    return ((r >> (8-f->var.red.length)) << f->var.red.offset) |
           ((g >> (8-f->var.green.length)) << f->var.green.offset) |
           ((b >> (8-f->var.blue.length)) << f->var.blue.offset);
}

static void make_lut(Frontend *f)
{
    unsigned i, r, g, b;
    for (i=0; i<65536; i++) {
        r = (i >> 11) & 31; g = (i >> 5) & 63; b = i & 31;
        f->lut[i] = fb_colour(f, (r << 3) | (r >> 2),
                              (g << 2) | (g >> 4), (b << 3) | (b >> 2));
    }
}

static int fb_open(Frontend *f, const char *path, int headless)
{
    f->fd = -1;
    f->headless = headless;
    if (headless) {
        f->width = MAX_WIDTH; f->height = MAX_HEIGHT; f->bytes = 3;
        f->pitch = f->width * 3;
        f->var.red.offset = 0; f->var.green.offset = 8; f->var.blue.offset = 16;
        f->var.red.length = f->var.green.length = f->var.blue.length = 8;
        f->length = f->pitch * f->height;
        f->memory = calloc(1, f->length);
        if (!f->memory) return -1;
    } else {
        f->fd = open(path, O_RDWR);
        if (f->fd < 0) { perror(path); return -1; }
        if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) < 0 ||
            ioctl(f->fd, FBIOGET_FSCREENINFO, &f->fix) < 0) {
            perror("framebuffer info"); return -1;
        }
        f->width = f->var.xres; f->height = f->var.yres;
        f->bytes = f->var.bits_per_pixel / 8;
        f->pitch = f->fix.line_length;
        f->xoffset = f->var.xoffset; f->yoffset = f->var.yoffset;
        if (f->width < 16 || f->width > MAX_WIDTH || f->height < 16 ||
            f->height > MAX_HEIGHT || f->var.nonstd ||
            f->fix.type != FB_TYPE_PACKED_PIXELS || f->fix.visual != FB_VISUAL_TRUECOLOR ||
            (f->var.bits_per_pixel != 16 && f->var.bits_per_pixel != 24 && f->var.bits_per_pixel != 32) ||
            f->var.red.length < 1 || f->var.red.length > 8 ||
            f->var.green.length < 1 || f->var.green.length > 8 ||
            f->var.blue.length < 1 || f->var.blue.length > 8 ||
            f->var.red.msb_right || f->var.green.msb_right || f->var.blue.msb_right ||
            f->var.red.offset + f->var.red.length > f->var.bits_per_pixel ||
            f->var.green.offset + f->var.green.length > f->var.bits_per_pixel ||
            f->var.blue.offset + f->var.blue.length > f->var.bits_per_pixel ||
            f->var.xoffset > f->var.xres_virtual ||
            f->var.xres > f->var.xres_virtual - f->var.xoffset ||
            f->var.yoffset > f->var.yres_virtual ||
            f->var.yres > f->var.yres_virtual - f->var.yoffset ||
            (unsigned long)(f->xoffset + f->width) * f->bytes > f->fix.line_length) {
            fprintf(stderr, "Unsupported fbdev layout; need packed truecolour <=1024x768.\n");
            return -1;
        }
        f->length = f->fix.smem_len;
        if (!f->length || (unsigned long)(f->yoffset + f->height) > f->length / f->fix.line_length) {
            fprintf(stderr, "Framebuffer mapping is smaller than its visible area.\n"); return -1;
        }
        f->memory = mmap(NULL, f->length, PROT_READ|PROT_WRITE, MAP_SHARED, f->fd, 0);
        if (f->memory == MAP_FAILED) { f->memory = NULL; perror("mmap fbdev"); return -1; }
    }
    make_lut(f);
    fprintf(stderr, "fbdev: %dx%d, %d bpp, stride %d, RGB offsets %u/%u/%u\n",
            f->width, f->height, f->bytes*8, f->pitch,
            f->var.red.offset, f->var.green.offset, f->var.blue.offset);
    return 0;
}

static void fb_close(Frontend *f)
{
    int i;
    for (i=0; i<f->ninput; i++) {
        ioctl(f->input[i], EVIOCGRAB, 0);
        close(f->input[i]);
    }
    f->ninput = 0;
    if (f->memory) {
        if (f->headless) free(f->memory); else munmap(f->memory, f->length);
        f->memory = NULL;
    }
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
}

/* Write bytewise: RGB24 scanlines and cursor positions may be unaligned. */
static void store_pixel(unsigned char *p, uint32 v, int bytes)
{
    p[0] = v; p[1] = v >> 8;
    if (bytes >= 3) p[2] = v >> 16;
    if (bytes == 4) p[3] = v >> 24;
}

static void status_text(Frontend *f, const char *text, int y, int scale)
{
    int n, row, bit, dx, dy, x;
    size_t length=strlen(text);
    unsigned char c;
    uint32 colour=fb_colour(f,220,232,240);
    if (length > (size_t)(f->width/(8*scale))) length=f->width/(8*scale);
    x=(f->width-(int)length*8*scale)/2;
    for (n=0;n<(int)length;n++) {
        c=text[n]; if(c<32 || c>126) c='?';
        for(row=0;row<16;row++) for(bit=0;bit<8;bit++)
            if(font8x16[c-32][row] & (0x80>>bit))
                for(dy=0;dy<scale;dy++) for(dx=0;dx<scale;dx++) {
                    int px=x+(n*8+bit)*scale+dx, py=y+row*scale+dy;
                    if(px>=0 && px<f->width && py>=0 && py<f->height)
                        store_pixel(f->memory+(py+f->yoffset)*f->pitch+(px+f->xoffset)*f->bytes,colour,f->bytes);
                }
    }
}

static void status_screen(Frontend *f, const char *message)
{
    int x,y;
    uint32 colour=fb_colour(f,12,24,36);
    if (!f->memory) return;
    for(y=0;y<f->height;y++) for(x=0;x<f->width;x++)
        store_pixel(f->memory+(y+f->yoffset)*f->pitch+(x+f->xoffset)*f->bytes,colour,f->bytes);
    status_text(f,"Remote Desktop",f->height/2-50,2);
    status_text(f,message,f->height/2+10,1);
}

static int dump_frame(Frontend *f, const char *path)
{
    FILE *out=fopen(path,"wb"); int ok;
    if(!out) return -1;
    fprintf(out,"P6\n%d %d\n255\n",f->width,f->height);
    ok=fwrite(f->memory,1,f->length,out)==f->length;
    if(fclose(out)) ok=0;
    return ok ? 0 : -1;
}

/* Four RGB24 pixels become three aligned word stores on little-endian ARM.
 * Byte stores handle unaligned edges. No unaligned ARM word accesses. */
static void convert_row(Frontend *f, unsigned char *dst, const uint16 *src, int n)
{
    uint32 a,b,c,d;
    if (f->bytes == 3) {
        while (n && ((unsigned long)dst & 3)) {
            a=f->lut[*src++]; dst[0]=a; dst[1]=a>>8; dst[2]=a>>16;
            dst+=3; n--;
        }
        while (n>=4) {
            a=f->lut[src[0]]; b=f->lut[src[1]];
            c=f->lut[src[2]]; d=f->lut[src[3]];
            ((uint32 *)dst)[0]=a | (b<<24);
            ((uint32 *)dst)[1]=(b>>8) | (c<<16);
            ((uint32 *)dst)[2]=(c>>16) | (d<<8);
            dst+=12; src+=4; n-=4;
        }
        while (n--) {
            a=f->lut[*src++]; dst[0]=a; dst[1]=a>>8; dst[2]=a>>16; dst+=3;
        }
    } else {
        while (n--) { store_pixel(dst,f->lut[*src++],f->bytes); dst+=f->bytes; }
    }
}

#ifdef L300_PERF_TEST
static unsigned long test_present_pixels, test_present_calls;
#endif
static void present(rdpInst *inst, int x, int y, int w, int h)
{
    Frontend *f=front(inst); GDI *g=GET_GDI(inst);
    int xx,yy,right,bottom,left,top,cr,cb,cx,cy;
    uint32 pixel,cp,a,r,green,b;
    const uint16 *src; unsigned char *dst; Cursor *c=f->cursor;
    if (!g || !f->memory || w<=0 || h<=0) return;
    if (x < -MAX_WIDTH || y < -MAX_HEIGHT || x>g->width || y>g->height) return;
    if (w>MAX_WIDTH*2) w=MAX_WIDTH*2;
    if (h>MAX_HEIGHT*2) h=MAX_HEIGHT*2;
    right=x+w; bottom=y+h;
    if (x<0) x=0;
    if (y<0) y=0;
    if (right>g->width) right=g->width;
    if (bottom>g->height) bottom=g->height;
    if (right>f->width) right=f->width;
    if (bottom>f->height) bottom=f->height;
    if (x>=right || y>=bottom) return;
#ifdef L300_PERF_TEST
    test_present_pixels+=(right-x)*(bottom-y); test_present_calls++;
#endif
    for (yy=y; yy<bottom; yy++) {
        src=(uint16 *)g->primary_buffer+yy*g->width+x;
        dst=f->memory+(yy+f->yoffset)*f->pitch+(x+f->xoffset)*f->bytes;
        convert_row(f,dst,src,right-x);
    }
    /* Cursor work is bounded by its intersection, never by desktop area.
     * Restore from the authoritative GDI backing store; no framebuffer reads. */
    if (!c || f->hidden) return;
    left=f->mouse_x-c->hotx; top=f->mouse_y-c->hoty;
    cr=left+c->width; cb=top+c->height;
    if (left<x) left=x;
    if (top<y) top=y;
    if (cr>right) cr=right;
    if (cb>bottom) cb=bottom;
    if (left>=cr || top>=cb) return;
    for (yy=top; yy<cb; yy++) {
        cy=yy-f->mouse_y+c->hoty;
        src=(uint16 *)g->primary_buffer+yy*g->width+left;
        dst=f->memory+(yy+f->yoffset)*f->pitch+(left+f->xoffset)*f->bytes;
        for (xx=left; xx<cr; xx++,src++,dst+=f->bytes) {
            cx=xx-f->mouse_x+c->hotx; cp=c->pixels[cy*c->width+cx]; a=cp>>24;
            if (!a) continue;
            r=(cp>>16)&255; green=(cp>>8)&255; b=cp&255;
            if (a!=255) {
                r=(r*a+(((*src>>11)*255)/31)*(255-a))/255;
                green=(green*a+((((*src>>5)&63)*255)/63)*(255-a))/255;
                b=(b*a+(((*src&31)*255)/31)*(255-a))/255;
            }
            pixel=fb_colour(f,r,green,b); store_pixel(dst,pixel,f->bytes);
        }
    }
}

static void cursor_rect(rdpInst *inst, Cursor *c, int x, int y)
{
    if (c) present(inst, x-c->hotx, y-c->hoty, c->width, c->height);
}
static void move_cursor(rdpInst *inst, int x, int y)
{
    Frontend *f = front(inst);
    GDI *g = GET_GDI(inst);
    int oldx=f->mouse_x, oldy=f->mouse_y;
    int w=g ? g->width : f->width, h=g ? g->height : f->height;
    f->mouse_x = x < 0 ? 0 : (x >= w ? w-1 : x);
    f->mouse_y = y < 0 ? 0 : (y >= h ? h-1 : y);
    if (oldx==f->mouse_x && oldy==f->mouse_y) return;
    if (f->cursor && !f->hidden) {
        Cursor *c=f->cursor;
        int left=oldx<f->mouse_x ? oldx : f->mouse_x;
        int top=oldy<f->mouse_y ? oldy : f->mouse_y;
        int width=c->width+abs(f->mouse_x-oldx), height=c->height+abs(f->mouse_y-oldy);
        if (width*height<=2*c->width*c->height)
            present(inst,left-c->hotx,top-c->hoty,width,height);
        else {
            cursor_rect(inst,c,oldx,oldy);
            cursor_rect(inst,c,f->mouse_x,f->mouse_y);
        }
    }
}
static void set_cursor(rdpInst *inst, RD_HCURSOR handle)
{
    Frontend *f=front(inst); Cursor *old=f->cursor;
    f->cursor = (Cursor *)handle; f->hidden=0;
    cursor_rect(inst, old, f->mouse_x, f->mouse_y);
    cursor_rect(inst, f->cursor, f->mouse_x, f->mouse_y);
}
static RD_HCURSOR create_cursor(rdpInst *inst, unsigned x, unsigned y, int w, int h,
                               uint8 *andmask, uint8 *xormask, int bpp)
{
    Cursor *c; CLRCONV conv;
    if (w <= 0 || h <= 0 || w > MAX_CURSOR || h > MAX_CURSOR ||
        x >= (unsigned)w || y >= (unsigned)h || !andmask || !xormask ||
        (bpp != 1 && bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32))
        return NULL;
    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->width=w; c->height=h; c->hotx=x; c->hoty=y;
    conv = *GET_GDI(inst)->clrconv; conv.alpha=1;
    gdi_alpha_cursor_convert((uint8 *)c->pixels, xormask, andmask, w, h, bpp, &conv);
    return (RD_HCURSOR)c;
}
static void default_cursor(rdpInst *inst) { set_cursor(inst, (RD_HCURSOR)&front(inst)->arrow); }
static void destroy_cursor(rdpInst *inst, RD_HCURSOR handle)
{
    Cursor *c=(Cursor *)handle;
    if (c == front(inst)->cursor) default_cursor(inst);
    if (c && c != &front(inst)->arrow) free(c);
}
static void hide_cursor(rdpInst *inst)
{
    Frontend *f=front(inst); f->hidden=1;
    cursor_rect(inst, f->cursor, f->mouse_x, f->mouse_y);
}
static void begin_update(rdpInst *inst)
{
    GDI *g=GET_GDI(inst);
    if (g) g->primary->hdc->hwnd->invalid->null=1;
}
static void end_update(rdpInst *inst)
{
    GDI *g=GET_GDI(inst); HGDI_RGN r;
    if (!g) return;
    r=g->primary->hdc->hwnd->invalid;
    if (!r->null) {
        HGDI_WND wnd=g->primary->hdc->hwnd;
        if (wnd->dirty_overflow || !wnd->dirty_count)
            present(inst,r->x,r->y,r->w,r->h);
        else {
            int i;
            for (i=0;i<wnd->dirty_count;i++) {
                HGDI_RGN d=&wnd->dirty[i];
                present(inst,d->x,d->y,d->w,d->h);
            }
        }
        r->null=1;
    }
}
static void log_error(rdpInst *inst, const char *text)
{ fprintf(stderr, "RDP: %s\n", text); front(inst)->error=1; }
static void log_message(rdpInst *inst, const char *text)
{ (void)inst; fprintf(stderr, "RDP: %s\n", text); }
static void bell(rdpInst *inst) { (void)inst; }
static uint32 toggles(rdpInst *inst) { return front(inst)->toggles; }
static int ui_select(rdpInst *inst, int fd) { (void)inst; (void)fd; return running; }
static void channel_data(rdpInst *inst, int id, char *data, int size, int flags, int total)
{ (void)inst; (void)id; (void)data; (void)size; (void)flags; (void)total; }

static int geometry_ok(rdpInst *inst)
{
    Frontend *f=front(inst);
    return inst->settings->width >= 16 && inst->settings->height >= 16 &&
           inst->settings->width <= f->width && inst->settings->height <= f->height;
}
static void resize_window(rdpInst *inst)
{
    if (!geometry_ok(inst)) {
        fprintf(stderr, "Server desktop exceeds framebuffer limits.\n");
        front(inst)->error=1; running=0; return;
    }
    gdi_free(inst);
    gdi_init(inst, CLRBUF_16BPP);
    memset(GET_GDI(inst)->primary_buffer, 0, inst->settings->width*inst->settings->height*2);
}

static int normalize_pin(const char *s, char out[65])
{
    int n=0;
    while (*s) {
        if (*s != ':') {
            if (!isxdigit((unsigned char)*s) || n >= 64) return -1;
            out[n++] = tolower((unsigned char)*s);
        }
        s++;
    }
    out[n]=0;
    return n == 64 ? 0 : -1;
}
static RD_BOOL certificate(rdpInst *inst, const char *fp, const char *subject,
                           const char *issuer, RD_BOOL verified)
{
    char normalized[65];
    (void)issuer; (void)verified; /* Old core does not configure a trusted CA store. */
    if (!normalize_pin(fp, normalized) && front(inst)->cert_pin[0] &&
        !strcmp(normalized, front(inst)->cert_pin)) return True;
    fprintf(stderr, "Server certificate: %s\nSHA256: %s\n", subject, fp);
    fprintf(stderr, "Certificate pin missing or different. Verify it with the server administrator,\n"
                    "then supply --cert-sha256 HEX (64 hex digits; colons optional).\n");
    front(inst)->error=66;
    return False;
}
static RD_BOOL authenticate(rdpInst *inst)
{
    if (!inst->settings->username[0] || !inst->settings->password[0]) {
        fprintf(stderr, "NLA needs -u USER and a password (prompt or --password-file).\n");
        return False;
    }
    return True;
}

static void defaults(rdpSet *s)
{
    memset(s, 0, sizeof(*s));
    s->width=1024; s->height=768; s->tcp_port_rdp=3389;
    strcpy(s->hostname, "l300");
    s->server_depth=16; s->rdp_version=5; s->encryption=1;
    s->tls_security=1; /* Select NLA or standard RDP explicitly. No downgrade. */
    s->bitmap_cache=1; s->bitmap_compression=1; s->software_gdi=1;
    s->new_cursors=1; s->mouse_motion=1;
    s->keyboard_layout=0x409; s->keyboard_type=4; s->keyboard_functionkeys=12;
    s->performanceflags=PERF_DISABLE_WALLPAPER|PERF_DISABLE_FULLWINDOWDRAG|
                        PERF_DISABLE_MENUANIMATIONS|PERF_DISABLE_THEMING;
}

static void callbacks(rdpInst *inst, Frontend *f)
{
    int x,y;
    inst->param1=f; inst->param2=NULL; inst->param3=NULL; inst->param4=NULL;
    inst->ui_error=log_error; inst->ui_warning=log_message; inst->ui_unimpl=log_message;
    inst->ui_begin_update=begin_update; inst->ui_end_update=end_update;
    inst->ui_bell=bell; inst->ui_get_toggle_keys_state=toggles;
    inst->ui_select=ui_select; inst->ui_resize_window=resize_window;
    inst->ui_create_cursor=create_cursor; inst->ui_destroy_cursor=destroy_cursor;
    inst->ui_set_cursor=set_cursor; inst->ui_set_null_cursor=hide_cursor;
    inst->ui_set_default_cursor=default_cursor; inst->ui_move_pointer=move_cursor;
    inst->ui_channel_data=channel_data; inst->ui_authenticate=authenticate;
    inst->ui_check_certificate=certificate;
    f->arrow.width=12; f->arrow.height=18;
    for (y=0; y<18; y++) for (x=0; x<12; x++)
        if (x <= y/2 && y < 16)
            f->arrow.pixels[y*12+x]=(x==0 || x==y/2 || y==15) ? 0xff000000 : 0xffffffff;
    f->cursor=&f->arrow;
    resize_window(inst); /* Installs all drawing callbacks from software GDI. */
}

static int open_inputs(Frontend *f, int grab)
{
    int i,fd,kind;
    char path[64], name[128];
    unsigned long bits[(EV_MAX+BITS_PER_LONG)/BITS_PER_LONG];
    for (i=0; i<L300_MAX_INPUT; i++) {
        snprintf(path, sizeof(path), "/dev/event%d", i);
        fd=open(path, O_RDONLY|O_NONBLOCK);
        if (fd < 0) {
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            fd=open(path, O_RDONLY|O_NONBLOCK);
        }
        if (fd < 0) continue;
        memset(bits, 0, sizeof(bits));
        if (fd >= FD_SETSIZE || ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits) < 0) { close(fd); continue; }
        kind = BIT_TEST(bits, EV_KEY) || BIT_TEST(bits, EV_REL);
        if (!kind) { close(fd); continue; }
        memset(name, 0, sizeof(name)); ioctl(fd, EVIOCGNAME(sizeof(name)-1), name);
        fprintf(stderr, "input: %s %s\n", path, name);
        strcpy(f->input_path[f->ninput],path);
        diagnose_input(f,f->ninput,fd,name,bits);
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
            perror("exclusive input grab"); close(fd); return -1;
        }
        f->input[f->ninput++]=fd;
    }
    return f->ninput ? 0 : -1;
}

static int scan_code(unsigned code, int *extended)
{
    *extended=0;
    if (code >= 1 && code <= 83) return code;
    if (code >= 86 && code <= 88) return code;
    *extended=1;
    switch (code) {
    case KEY_KPENTER: return 0x1c; case KEY_RIGHTCTRL: return 0x1d;
    case KEY_KPSLASH: return 0x35; case KEY_SYSRQ: return 0x37;
    case KEY_RIGHTALT: return 0x38; case KEY_HOME: return 0x47;
    case KEY_UP: return 0x48; case KEY_PAGEUP: return 0x49;
    case KEY_LEFT: return 0x4b; case KEY_RIGHT: return 0x4d;
    case KEY_END: return 0x4f; case KEY_DOWN: return 0x50;
    case KEY_PAGEDOWN: return 0x51; case KEY_INSERT: return 0x52;
    case KEY_DELETE: return 0x53; case KEY_LEFTMETA: return 0x5b;
    case KEY_RIGHTMETA: return 0x5c; case KEY_COMPOSE: return 0x5d;
    default: return 0;
    }
}

static void handle_input(rdpInst *inst, int slot, struct input_event *ev)
{
    Frontend *f=front(inst);
    int scan,ext,flags=0,n,diag=diagnose_event(f,slot,ev);
    if (ev->type == EV_REL) {
        if (ev->code == REL_X || ev->code == REL_Y) {
            /* Clamp relative movement before addition to avoid integer overflow. */
            n=ev->value; if (n > 2048) n=2048; if (n < -2048) n=-2048;
            move_cursor(inst, f->mouse_x+(ev->code==REL_X ? n : 0),
                              f->mouse_y+(ev->code==REL_Y ? n : 0));
            if(diag) diagnose_send(f,slot,PTRFLAGS_MOVE);
            inst->rdp_send_input_mouse(inst, PTRFLAGS_MOVE, f->mouse_x, f->mouse_y);
        } else if (ev->code == REL_WHEEL) {
            n=ev->value; if (n > 8) n=8; if (n < -8) n=-8;
            while (n) {
                flags=PTRFLAGS_WHEEL | (n > 0 ? 120 : (PTRFLAGS_WHEEL_NEGATIVE|0x188));
                if(diag) diagnose_send(f,slot,flags);
                inst->rdp_send_input_mouse(inst, flags, f->mouse_x, f->mouse_y);
                n += n > 0 ? -1 : 1;
            }
        }
        return;
    }
    if (ev->type != EV_KEY) return;
    switch (ev->code) {
    case BTN_LEFT: flags=PTRFLAGS_BUTTON1; break;
    case BTN_RIGHT: flags=PTRFLAGS_BUTTON2; break;
    case BTN_MIDDLE: flags=PTRFLAGS_BUTTON3; break;
    }
    if (flags) {
        if (ev->value) f->buttons |= flags; else f->buttons &= ~flags;
        if(diag) diagnose_send(f,slot,flags | (ev->value ? PTRFLAGS_DOWN : 0));
        inst->rdp_send_input_mouse(inst, flags | (ev->value ? PTRFLAGS_DOWN : 0), f->mouse_x, f->mouse_y);
        return;
    }
    if (ev->code < sizeof(f->keys)) f->keys[ev->code]=(ev->value!=0);
    if (ev->code == KEY_F12 && ev->value &&
        (f->keys[KEY_LEFTCTRL] || f->keys[KEY_RIGHTCTRL]) &&
        (f->keys[KEY_LEFTALT] || f->keys[KEY_RIGHTALT])) { f->local_exit=1; running=0; return; }
    if (ev->value == 1) {
        if (ev->code == KEY_SCROLLLOCK) f->toggles ^= 1;
        if (ev->code == KEY_NUMLOCK) f->toggles ^= 2;
        if (ev->code == KEY_CAPSLOCK) f->toggles ^= 4;
    }
    if (ev->code == KEY_PAUSE) {
        /* RDP's two-event representation of the E1 Pause sequence. */
        if (ev->value == 1) {
            inst->rdp_send_input_scancode(inst, 0, 0, 0x1d);
            inst->rdp_send_input_scancode(inst, 0, 0, 0x45);
            inst->rdp_send_input_scancode(inst, 1, 0, 0x1d);
            inst->rdp_send_input_scancode(inst, 1, 0, 0x45);
        }
        return;
    }
    scan=scan_code(ev->code, &ext);
    if (scan) inst->rdp_send_input_scancode(inst, ev->value==0, ext, scan);
}

static int event_loop(rdpInst *inst)
{
    Frontend *f=front(inst);
    void *reads[32], *writes[32];
    int nr,nw,maxfd,i,fd,ret; ssize_t got;
    fd_set rfds,wfds; struct timeval timeout;
    struct input_event events[32];
    while (running) {
        nr=nw=0; maxfd=-1; FD_ZERO(&rfds); FD_ZERO(&wfds);
        if (inst->rdp_get_fds(inst, reads, &nr, writes, &nw) != 0 || nr > 32 || nw > 32) return 1;
        for (i=0; i<nr+nw; i++) {
            fd=(int)(long)(i<nr ? reads[i] : writes[i-nr]);
            if (fd < 0 || fd >= FD_SETSIZE) return 1;
            if (i<nr) FD_SET(fd,&rfds); else FD_SET(fd,&wfds);
            if (fd>maxfd) maxfd=fd;
        }
        for (i=0; i<f->ninput; i++) {
            FD_SET(f->input[i], &rfds);
            if (f->input[i]>maxfd) maxfd=f->input[i];
        }
        timeout.tv_sec=freerdp_has_pending_data(inst) ? 0 : 1; timeout.tv_usec=0;
        ret=select(maxfd+1, &rfds, &wfds, NULL, &timeout);
        if (ret<0) { if (errno==EINTR) continue; perror("select"); return 1; }
        alarm(f->timeout); /* Bound input writes as well as partial packet reads. */
        for (i=0; i<f->ninput; i++) if (FD_ISSET(f->input[i], &rfds)) {
            got=read(f->input[i], events, sizeof(events));
            if (got < 0 && (errno==EAGAIN || errno==EINTR)) continue;
            if (got <= 0 || got % sizeof(events[0])) {
                fprintf(stderr, "Input device disconnected; reconnect and restart client.\n"); return 1;
            }
            for (fd=0; fd<(int)(got/sizeof(events[0])); fd++) handle_input(inst, i, &events[fd]);
        }
        if (!running) { alarm(0); break; }
        ret=inst->rdp_check_fds(inst);
        alarm(0);
        if (ret != 0) return inst->disc_reason > 2 || f->error ? 1 : 0;
    }
    return f->error ? 1 : 0;
}

static int copy_arg(char *dst, size_t length, const char *value)
{
    if (strlen(value)>=length) { fprintf(stderr,"Argument is too long.\n"); return -1; }
    strcpy(dst,value); return 0;
}
static int read_password(rdpSet *s, const char *file)
{
    int fd; FILE *stream; struct stat st; struct termios old,new;
    char buffer[128]; size_t n; int tty=0,ok;
    if (file) {
        fd=open(file,O_RDONLY|O_NOFOLLOW);
        if (fd<0) { perror(file); return -1; }
        if (fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || (st.st_mode&077) || st.st_uid!=getuid()) {
            fprintf(stderr,"Password file must be owned by this user with mode 600 or 400.\n"); close(fd); return -1;
        }
        stream=fdopen(fd,"r");
    } else {
        stream=fopen("/dev/tty","r+");
        if (stream) {
            fd=fileno(stream);
            if (tcgetattr(fd,&old)==0) { new=old; new.c_lflag &= ~ECHO; tty=tcsetattr(fd,TCSAFLUSH,&new)==0; }
            if (!tty) { fclose(stream); stream=NULL; }
        }
        if (stream) { fputs("RDP password: ",stream); fflush(stream); }
    }
    if (!stream) { fprintf(stderr,"No password terminal; use --password-file.\n"); return -1; }
    buffer[0]=0;
    ok=fgets(buffer,sizeof(buffer),stream)!=NULL;
    if (tty) { tcsetattr(fd,TCSAFLUSH,&old); fputs("\n",stream); }
    fclose(stream);
    if (!ok || !running) { memset(buffer,0,sizeof(buffer)); return -1; }
    n=strcspn(buffer,"\r\n"); buffer[n]=0;
    if (n>=sizeof(s->password)) { memset(buffer,0,sizeof(buffer)); return -1; }
    memcpy(s->password,buffer,n+1); memset(buffer,0,sizeof(buffer));
    return 0;
}

static int self_test(void); /* Includes library, raster, input, and ABI checks. */
static void help(void)
{
    puts("fbfreerdp " PACKAGE_VERSION " (ARM926 OABI, static)\n"
         "Usage: fbfreerdp [options] IPv4\n"
         "  -u USER  -d DOMAIN  -g WxH  -t PORT  -k HEX_LAYOUT\n"
         "  --password-file FILE   owner-only file; otherwise prompt on /dev/tty\n"
         "  --sec tls|nla|rdp      default tls; nla is legacy CredSSP\n"
         "  --cert-sha256 HEX      trusted server certificate pin for TLS/NLA\n"
         "  --fb /dev/fb0         existing 16/24/32-bit truecolour mode\n"
         "  --timeout SECONDS     connect/incomplete-packet timeout (default 20)\n"
         "  --probe               inspect framebuffer/input, no connection or drawing\n"
         "  --self-test           CPU/crypto/Unicode/GDI/input tests, no devices\n"
         "  --headless            memory framebuffer for integration tests\n"
         "  --dump-frame FILE     save headless RGB frame as PPM on disconnect\n"
         "  --version             show build and linked crypto versions\n"
         "  --status TEXT         paint a status screen; no connection or input\n"
         "  --appliance           TLS/pin only, server-side login, clear screen on exit\n"
         "  --client-name NAME    distinct RDP client hostname for this device\n"
         "  --mouse-diag PATH     bounded raw/capability/send logs; '-' stderr only,\n"
         "                        or mirror to existing serial device (no setup)\n"
         "Exit remote session: Ctrl+Alt+F12. USB devices must be attached at startup.\n"
         "16-bit RDP, no X11/DirectFB, audio, redirection, RemoteFX or dynamic plugins.");
}

int main(int argc,char **argv)
{
    rdpSet settings; Frontend *f; rdpInst *inst=NULL;
    const char *fbpath="/dev/fb0", *password_file=NULL, *dump_file=NULL, *status=NULL, *diag_path=NULL;
    int i,rc=1,headless=0,probe=0,timeout=20,connected=0,appliance=0;
    struct in_addr addr; char tail; char *end; long value;
    struct sigaction action;
    if (argc==2 && !strcmp(argv[1],"--self-test")) return self_test();
    if (argc==2 && !strcmp(argv[1],"--version")) {
        puts("fbfreerdp " PACKAGE_VERSION "\nGCC " __VERSION__ "\n" OPENSSL_VERSION_TEXT);
        puts("ARM926EJ-S; APCS-GNU/OABI; little endian; software floating point; static"); return 0;
    }
    f=calloc(1,sizeof(*f)); if (!f) return 1; f->fd=-1; f->diag_fd=-1;
    defaults(&settings);
    for (i=1; i<argc; i++) {
        const char *arg=argv[i],*v;
        if (!strcmp(arg,"--help") || !strcmp(arg,"-h")) { help(); rc=0; goto done; }
        if (!strcmp(arg,"--headless")) { headless=1; continue; }
        if (!strcmp(arg,"--probe")) { probe=1; continue; }
        if (!strcmp(arg,"--appliance")) { appliance=1; continue; }
        if (arg[0]!='-') { if (settings.server[0] || copy_arg(settings.server,sizeof(settings.server),arg)) goto bad; continue; }
        if (++i==argc) goto bad; v=argv[i];
        if (!strcmp(arg,"-u")) { if (copy_arg(settings.username,sizeof(settings.username),v)) goto bad; }
        else if (!strcmp(arg,"-d")) { if (copy_arg(settings.domain,sizeof(settings.domain),v)) goto bad; }
        else if (!strcmp(arg,"--fb")) fbpath=v;
        else if (!strcmp(arg,"--password-file")) password_file=v;
        else if (!strcmp(arg,"--dump-frame")) dump_file=v;
        else if (!strcmp(arg,"--status")) status=v;
        else if (!strcmp(arg,"--mouse-diag")) diag_path=v;
        else if (!strcmp(arg,"--client-name")) { if(copy_arg(settings.hostname,sizeof(settings.hostname),v)) goto bad; }
        else if (!strcmp(arg,"--cert-sha256")) { if (normalize_pin(v,f->cert_pin)) goto bad; }
        else if (!strcmp(arg,"-g")) {
            if (sscanf(v,"%dx%d%c",&settings.width,&settings.height,&tail)!=2 ||
                settings.width<16 || settings.width>MAX_WIDTH || settings.height<16 || settings.height>MAX_HEIGHT) goto bad;
        } else if (!strcmp(arg,"-t") || !strcmp(arg,"--timeout") || !strcmp(arg,"-k")) {
            errno=0; value=strtol(v,&end,!strcmp(arg,"-k")?16:10);
            if (errno || !*v || *end || value<=0 || value>65535) goto bad;
            if (!strcmp(arg,"-t")) settings.tcp_port_rdp=value;
            else if (!strcmp(arg,"-k")) settings.keyboard_layout=value;
            else { if (value>300) goto bad; timeout=value; }
        } else if (!strcmp(arg,"--sec")) {
            settings.rdp_security=!strcmp(v,"rdp"); settings.tls_security=!strcmp(v,"tls"); settings.nla_security=!strcmp(v,"nla");
            if (!settings.rdp_security && !settings.tls_security && !settings.nla_security) goto bad;
        } else goto bad;
    }
    if (dump_file && !headless) goto bad;
    f->timeout=timeout;
    if (appliance && (settings.username[0] || settings.domain[0] || password_file ||
        !settings.tls_security || settings.nla_security || settings.rdp_security || !f->cert_pin[0])) goto bad;
    if (!probe && !status && (!settings.server[0] || !inet_aton(settings.server,&addr))) {
        fprintf(stderr,"Supply a numeric IPv4 server address; DNS/NSS is excluded.\n"); goto bad;
    }
    if(diag_path) {
        f->mouse_diag=1;
        if(strcmp(diag_path,"-")) {
            f->diag_fd=open(diag_path,O_WRONLY|O_NONBLOCK|O_NOCTTY);
            if(f->diag_fd<0) { perror("mouse diagnostic mirror"); goto done; }
        }
        mouse_diag_line(f,"MOUSE-DIAG START input_event_bytes=%u; raw values unchanged; serial is best-effort",
                        (unsigned)sizeof(struct input_event));
    }
    if (fb_open(f,fbpath,headless)<0) goto done;
    if (status) {
        status_screen(f,status); rc=dump_file && dump_frame(f,dump_file) ? 1 : 0; goto done;
    }
    if (probe) {
        rc=headless ? 0 : (open_inputs(f,0)<0);
        if (rc) fprintf(stderr,"No USB evdev input devices found.\n");
        else puts("Device probe passed (no framebuffer pixels changed).");
        goto done;
    }
    if (settings.width>f->width || settings.height>f->height) { fprintf(stderr,"Desktop is larger than framebuffer.\n"); goto done; }
    /* Interrupt password reads so the normal path restores terminal echo. */
    memset(&action,0,sizeof(action)); action.sa_handler=stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL); sigaction(SIGTERM,&action,NULL); sigaction(SIGHUP,&action,NULL);
    if (settings.username[0]) {
        if (read_password(&settings,password_file)<0) goto done;
        settings.autologin=1;
    }
    signal(SIGPIPE,SIG_IGN); signal(SIGALRM,connect_timeout);
    if (!freerdp_global_init()) goto done;
    inst=freerdp_new(&settings); if (!inst) goto done_global;
    callbacks(inst,f);
    alarm(timeout);
    if (inst->rdp_connect(inst)!=0) { alarm(0); rc=f->error==66 ? 66 : 1; fprintf(stderr,"RDP connection failed.\n"); goto done_inst; }
    connected=1; alarm(0);
    if (!geometry_ok(inst) || !running) goto done_inst;
    if (GET_GDI(inst)->width != settings.width || GET_GDI(inst)->height != settings.height ||
        GET_GDI(inst)->srcBpp != settings.server_depth) resize_window(inst);
    if (!headless && open_inputs(f,1)<0) {
        fprintf(stderr,"No usable exclusive USB input; stop other input clients and retry.\n"); goto done_inst;
    }
    /* Core synchronizes toggle keys after Demand Active, not at TCP connect. */
    present(inst,0,0,settings.width,settings.height);
    fprintf(stderr,"Connected: %dx%d, server %d bpp, GDI RGB565. Ctrl+Alt+F12 exits.\n",
            settings.width,settings.height,settings.server_depth);
    fprintf(stderr,"L300 PERFORMANCE v1: packed RGB24, bounded damage, TLS pending-data dispatch.\n");
    rc=event_loop(inst);
    if(appliance && f->local_exit) rc=75;
done_inst:
    if (connected) {
        int ext,scan;
        /* Do not leave remote Ctrl/Alt held after the local exit chord. */
        for (i=1;i<256;i++) if (f->keys[i]) {
            scan=scan_code(i,&ext);
            if (scan) inst->rdp_send_input_scancode(inst,1,ext,scan);
        }
        if (dump_file) {
            if(dump_frame(f,dump_file)) { perror(dump_file); rc=1; }
        }
        inst->rdp_disconnect(inst);
    }
    /* Core cache teardown calls cursor/bitmap destructors; keep GDI alive. */
    {
        GDI *g=GET_GDI(inst); rdpInst cleanup;
        memset(&cleanup,0,sizeof(cleanup)); cleanup.param2=g;
        freerdp_free(inst); gdi_free(&cleanup);
    }
done_global:
    freerdp_global_finish();
done:
    memset(settings.password,0,sizeof(settings.password));
    if(appliance) status_screen(f,"Connecting to server...");
    if(f->diag_fd>=0) close(f->diag_fd);
    fb_close(f); free(f); return rc;
bad:
    help(); rc=2; goto done;
}

static unsigned test_mouse_count;
static uint16 test_mouse_flags,test_mouse_x,test_mouse_y;
static int capture_test_mouse(rdpInst *inst,uint16 flags,uint16 x,uint16 y)
{
    (void)inst;
    test_mouse_count++; test_mouse_flags=flags; test_mouse_x=x; test_mouse_y=y;
    return 0;
}

static int self_test(void)
{
    rdpSet settings; Frontend *f; rdpInst *inst;
    unsigned char digest[32],random[32],cipher[9]; RC4_KEY rc4;
    UNICONV *uc; char *utf16,*utf8; size_t length;
    int ext,failed=0; unsigned i;
    const unsigned char expected_sha[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    const unsigned char expected_rc4[9]={0xbb,0xf3,0x16,0xe8,0xd9,0x40,0xaf,0x0a,0xd3};
    struct rusage usage;
#define CHECK(test,label) do { if (!(test)) { fprintf(stderr,"FAIL: %s\n",label); failed++; } else printf("PASS: %s\n",label); } while (0)
    CHECK(sizeof(void*)==4 && sizeof(long)==4 && sizeof(struct input_event)==16,"32-bit userspace and 16-byte Linux input_event ABI");
    CHECK(sizeof(struct timeval)==8 && offsetof(struct input_event,type)==8 &&
          offsetof(struct input_event,code)==10 && offsetof(struct input_event,value)==12 &&
          sizeof(((struct input_event *)0)->type)==2 && sizeof(((struct input_event *)0)->code)==2 &&
          sizeof(((struct input_event *)0)->value)==4,
          "input_event field layout matches the vendor module DWARF ABI");
    SHA256((const unsigned char *)"abc",3,digest);
    CHECK(!memcmp(digest,expected_sha,32),"SHA256 known-answer vector");
    RC4_set_key(&rc4,3,(const unsigned char *)"Key");
    RC4(&rc4,9,(const unsigned char *)"Plaintext",cipher);
    CHECK(!memcmp(cipher,expected_rc4,9),"RC4 standard-RDP known-answer vector");
    CHECK(RAND_bytes(random,sizeof(random))==1,"OpenSSL entropy from kernel devices");
    uc=freerdp_uniconv_new();
    utf16=freerdp_uniconv_out(uc,"J\303\266rg \342\202\254 \360\237\230\200",&length);
    CHECK(utf16 && length==18,"UTF8 to UTF16LE including surrogate pair");
    utf8=utf16 ? freerdp_uniconv_in(uc,(unsigned char*)utf16,length) : NULL;
    CHECK(utf8 && !strcmp(utf8,"J\303\266rg \342\202\254 \360\237\230\200"),"Unicode round trip using static libiconv");
    free(utf16); free(utf8); freerdp_uniconv_free(uc);
    CHECK(scan_code(KEY_RIGHTCTRL,&ext)==0x1d && ext,"extended Ctrl scancode");
    CHECK(scan_code(KEY_A,&ext)==0x1e && !ext,"USB A to RDP scancode");
    CHECK(scan_code(KEY_KPENTER,&ext)==0x1c && ext,"extended keypad Enter");
    f=calloc(1,sizeof(*f)); if (!f) return 1; f->fd=-1;
    if (fb_open(f,NULL,1)<0) { free(f); return 1; }
    defaults(&settings); freerdp_global_init(); inst=freerdp_new(&settings);
    callbacks(inst,f); f->hidden=1;
    begin_update(inst); inst->ui_rect(inst,10,10,20,20,0xf800); end_update(inst);
    i=10*f->pitch+10*3;
    CHECK(f->memory[i]==255 && f->memory[i+1]==0 && f->memory[i+2]==0,"RDP RGB565 rectangle to RGB24 framebuffer");
    CHECK(f->memory[i-3]==0 && f->memory[i+20*3]==0,"dirty rectangle clipping");
    CHECK(f->lut[0x07e0]==0x00ff00 && f->lut[0x001f]==0xff0000,"RGB565 green and blue conversion");
    for (i=0;i<20;i++) { begin_update(inst); inst->ui_rect(inst,0,0,1024,768,i); end_update(inst); }
    CHECK(GET_GDI(inst)->bytesPerPixel==2,"1.5 MiB desktop backing buffer at 1024x768");
    {
        HGDI_WND wnd=GET_GDI(inst)->primary->hdc->hwnd;
        begin_update(inst);
        inst->ui_rect(inst,0,0,20,20,0xf800);
        inst->ui_rect(inst,1004,748,20,20,0x001f);
        CHECK(wnd->dirty_count==2 && !wnd->dirty_overflow,
              "distant damage stays two small rectangles");
        end_update(inst);
        CHECK(wnd->invalid->null,"completed update clears pending damage");
        begin_update(inst);
        inst->ui_rect(inst,10,10,20,20,0);
        inst->ui_rect(inst,10,10,20,20,0);
        CHECK(wnd->dirty_count==1,"duplicate damage coalesces once");
        end_update(inst);
    }
    /* Protect the hardware-tested colour quirk without changing GDI RGB565. */
    f->headless=0;
    CHECK(fb_colour(f,255,0,0)==0xff0000 && fb_colour(f,0,0,255)==0x0000ff,
          "physical L300 red/blue swap retained");
    f->headless=1;
    {
        struct input_event ev; unsigned before;
        int (*saved_mouse)(rdpInst *,uint16,uint16,uint16)=inst->rdp_send_input_mouse;
        inst->rdp_send_input_mouse=capture_test_mouse;
        f->mouse_x=100; f->mouse_y=100;
        memset(&ev,0,sizeof(ev)); ev.type=EV_REL; ev.code=REL_X; ev.value=3;
        handle_input(inst,0,&ev);
        ev.code=REL_Y; ev.value=-2; handle_input(inst,0,&ev);
        CHECK(test_mouse_count==2 && test_mouse_flags==PTRFLAGS_MOVE &&
              test_mouse_x==103 && test_mouse_y==98,"raw X/Y movement translation unchanged");
        ev.code=REL_WHEEL; ev.value=1; handle_input(inst,0,&ev);
        CHECK(test_mouse_flags==0x0278,"positive wheel remains +120");
        ev.value=-1; handle_input(inst,0,&ev);
        CHECK(test_mouse_flags==0x0388,"negative wheel remains signed 9-bit -120");
        before=test_mouse_count; ev.value=0; handle_input(inst,0,&ev);
        CHECK(test_mouse_count==before,"zero wheel input sends no RDP mouse event");
        ev.type=EV_KEY; ev.code=BTN_MIDDLE; ev.value=1; handle_input(inst,0,&ev);
        CHECK(test_mouse_flags==0xc000 && f->buttons==PTRFLAGS_BUTTON3,"middle button press state retained");
        ev.value=0; handle_input(inst,0,&ev);
        CHECK(test_mouse_flags==0x4000 && f->buttons==0,"middle button release state retained");
        f->mouse_diag=1; f->diag_fd=-1;
        strcpy(f->input_path[0],"synthetic-self-test");
        ev.type=EV_REL; ev.code=REL_WHEEL; ev.value=-1;
        before=test_mouse_count; handle_input(inst,0,&ev);
        CHECK(test_mouse_count==before+1 && test_mouse_flags==0x0388 && f->diag_lines==2,
              "diagnostics observe raw and outgoing events without changing translation");
        f->mouse_diag=0;
        inst->rdp_send_input_mouse=saved_mouse;
    }
    {
        rdpInst cleanup; memset(&cleanup,0,sizeof(cleanup)); cleanup.param2=GET_GDI(inst);
        freerdp_free(inst); gdi_free(&cleanup);
    }
    fb_close(f); free(f); freerdp_global_finish();
    if (!getrusage(RUSAGE_SELF,&usage)) printf("getrusage maxrss: %ld (emulator may include its own overhead)\n",usage.ru_maxrss);
    printf("Self-test: %s\n",failed ? "FAILED" : "PASS");
    return failed ? 1 : 0;
}
