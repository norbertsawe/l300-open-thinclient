/* Differential renderer/cursor/bitmap tests against the production algorithm. */
#define L300_PERF_TEST 1
#define main frontend_main
#include "../FreeRDP-old-master/l300/fbfreerdp.c"
#undef main
#include "reference-present.h"
#include <assert.h>

static unsigned rng=600;
static unsigned random_u(void) { rng=rng*1664525U+1013904223U; return rng; }
static void compare(rdpInst *inst, int x,int y,int w,int h)
{
    Frontend *f=front(inst); unsigned char *real=f->memory;
    unsigned char *expected=malloc(f->length);
    memcpy(expected,real,f->length);
    present(inst,x,y,w,h);
    f->memory=expected; reference_present(inst,x,y,w,h); f->memory=real;
    assert(!memcmp(real,expected,f->length)); free(expected);
}

int main(void)
{
    Frontend *f=calloc(1,sizeof(*f)); rdpSet s; rdpInst *inst; GDI *g;
    unsigned char *allocation,*expected,*saved; Cursor *c=calloc(1,sizeof(*c));
    int bytes,headless,i,j,pass;
    defaults(&s); freerdp_global_init(); inst=freerdp_new(&s);
    assert(fb_open(f,NULL,1)==0); callbacks(inst,f); g=GET_GDI(inst);
    free(f->memory);
    for(i=0;i<1024*768;i++) ((uint16 *)g->primary_buffer)[i]=random_u();
    c->width=32; c->height=32; c->hotx=7; c->hoty=9;
    for(i=0;i<1024;i++) c->pixels[i]=random_u();
    c->pixels[0]=0; c->pixels[1]=0xff00ff00; f->cursor=c;
    for(bytes=2;bytes<=4;bytes++) for(headless=0;headless<=1;headless++) {
        f->bytes=bytes; f->headless=headless; f->xoffset=1; f->yoffset=2;
        f->pitch=(1024+2)*bytes+3; f->length=(768+4)*f->pitch;
        f->var.red.offset=bytes==2?11:0; f->var.red.length=bytes==2?5:8;
        f->var.green.offset=bytes==2?5:8; f->var.green.length=bytes==2?6:8;
        f->var.blue.offset=bytes==2?0:16; f->var.blue.length=bytes==2?5:8;
        make_lut(f);
        allocation=malloc(f->length+128); memset(allocation,0x5a,f->length+128);
        f->memory=allocation+64; f->mouse_x=500; f->mouse_y=380;
        for(i=0;i<2;i++) { f->hidden=i; compare(inst,0,0,1024,768); }
        for(i=0;i<80;i++) {
            int x=(int)(random_u()%1150)-80, y=(int)(random_u()%850)-50;
            f->hidden=i%3==0; f->mouse_x=x; f->mouse_y=y;
            compare(inst,x-15,y-15,random_u()%200+1,random_u()%120+1);
        }
        for(i=0;i<64;i++) assert(allocation[i]==0x5a && allocation[f->length+64+i]==0x5a);
        free(allocation);
    }
    puts("PASS 492 differential render cases: RGB16/24/32, offsets, padded/unaligned rows, clipping, alpha cursors, physical R/B quirk");
    f->headless=1; f->xoffset=f->yoffset=0;
    assert(fb_open(f,NULL,1)==0); f->hidden=1;
    expected=malloc(f->length); saved=malloc(1024*768*2);
    present(inst,0,0,1024,768);
    test_present_pixels=test_present_calls=0;
    begin_update(inst); inst->ui_rect(inst,0,0,20,20,0xf800);
    inst->ui_rect(inst,1004,748,20,20,0x001f); end_update(inst);
    assert(test_present_pixels==800 && test_present_calls==2);
    end_update(inst); assert(test_present_pixels==800);
    puts("PASS sparse damage writes 800 pixels, not 786432; no repeated end-update writes");
    for(pass=0;pass<20;pass++) {
        begin_update(inst);
        for(i=0;i<100;i++) {
            int x=random_u()%1024,y=random_u()%768;
            inst->ui_rect(inst,x,y,random_u()%80+1,random_u()%60+1,random_u()&65535);
        }
        end_update(inst);
        memcpy(expected,f->memory,f->length);
        reference_present(inst,0,0,1024,768);
        assert(!memcmp(expected,f->memory,f->length));
    }
    begin_update(inst);
    for(i=0;i<65;i++) inst->ui_rect(inst,(i%16)*60,(i/16)*60,1,1,0xffff);
    assert(g->primary->hdc->hwnd->dirty_overflow); end_update(inst);
    memcpy(expected,f->memory,f->length); reference_present(inst,0,0,1024,768);
    assert(!memcmp(expected,f->memory,f->length));
    puts("PASS 2000 random drawing orders and 65-region overflow preserve all damage");
    f->hidden=0; f->mouse_x=500; f->mouse_y=380;
    present(inst,0,0,1024,768);
    for(i=0;i<80;i++) {
        int x=i%2?500+i:random_u()%1024,y=i%2?380+i:random_u()%768;
        move_cursor(inst,x,y); memcpy(expected,f->memory,f->length);
        reference_present(inst,0,0,1024,768);
        assert(!memcmp(expected,f->memory,f->length));
    }
    test_present_pixels=0; move_cursor(inst,f->mouse_x,f->mouse_y);
    assert(test_present_pixels==0);
    hide_cursor(inst); memcpy(expected,f->memory,f->length);
    reference_present(inst,0,0,1024,768); assert(!memcmp(expected,f->memory,f->length));
    puts("PASS 80 cursor moves, overlapping/distant restores, hide, and no-op movement");
    for(i=0;i<100;i++) {
        uint8 data[64*64*2]; GDI_IMAGE *bmp;
        int x=(int)(random_u()%1100)-40,y=(int)(random_u()%820)-30;
        for(j=0;j<(int)sizeof(data);j++) data[j]=random_u()>>24;
        if(i&1) gdi_SetClipRgn(g->primary->hdc,15,17,940,700);
        else gdi_SetNullClipRgn(g->primary->hdc);
        memcpy(saved,g->primary_buffer,1024*768*2);
        inst->ui_paint_bitmap(inst,x,y,61,59,64,64,data);
        memcpy(expected,g->primary_buffer,1024*768*2);
        memcpy(g->primary_buffer,saved,1024*768*2);
        bmp=(GDI_IMAGE *)inst->ui_create_bitmap(inst,64,64,data);
        gdi_BitBlt(g->primary->hdc,x,y,61,59,bmp->hdc,0,0,GDI_SRCCOPY);
        inst->ui_destroy_bitmap(inst,(RD_HBITMAP)bmp);
        assert(!memcmp(expected,g->primary_buffer,1024*768*2));
    }
    puts("PASS 100 RGB565 bitmap fast-copy cases match original allocation/BitBlt path including clipping");
    puts("ALL PERFORMANCE RENDER TESTS PASSED");
    return 0;
}
