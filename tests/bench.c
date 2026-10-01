/* Run the real frontend/GDI operations; timing is QEMU-host time, not board FPS. */
#define main frontend_main
#ifndef FRONTEND_SOURCE
#define FRONTEND_SOURCE "../FreeRDP-old-master/l300/fbfreerdp.c"
#endif
#include FRONTEND_SOURCE
#undef main
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*1000.0+t.tv_nsec/1000000.0;
}

int main(void)
{
    Frontend *f=calloc(1,sizeof(*f)); rdpSet s; rdpInst *inst;
    int k,i,n; double start; unsigned hash;
    unsigned char *copy=malloc(1024*768*3), bitmap[64*64*2];
    const char *names[]={"full_hidden","full_cursor","dirty_32","dirty_800x600",
                         "cursor_move","sparse_callbacks","bitmap_64","copy_full"};
    defaults(&s); freerdp_global_init(); inst=freerdp_new(&s);
    if(fb_open(f,NULL,1)) return 1;
    callbacks(inst,f);
    for(i=0;i<1024*768;i++) ((uint16 *)GET_GDI(inst)->primary_buffer)[i]=(i*7919)^0xa55a;
    memset(bitmap,0xa5,sizeof(bitmap));
    for(k=0;k<8;k++) {
        n=(k==2 || k==4 || k==6)?4000:80;
        f->hidden=k!=1 && k!=4; f->mouse_x=500; f->mouse_y=380;
        present(inst,0,0,1024,768);
        start=now_ms();
        for(i=0;i<n;i++) switch(k) {
        case 0: case 1: present(inst,0,0,1024,768); break;
        case 2: present(inst,113,117,32,32); break;
        case 3: present(inst,5,7,800,600); break;
        case 4: move_cursor(inst,500+(i&31),380+(i&15)); break;
        case 5:
            begin_update(inst);
            inst->ui_rect(inst,0,0,20,20,i);
            inst->ui_rect(inst,1004,748,20,20,i);
            end_update(inst); break;
        case 6: inst->ui_paint_bitmap(inst,100,100,64,64,64,64,bitmap); break;
        case 7: memcpy(copy,f->memory,f->length); break;
        }
        printf("BENCH %s iterations=%d ms_per_op=%.6f\n",names[k],n,(now_ms()-start)/n);
    }
    hash=0; for(i=0;i<(int)f->length;i++) hash=hash*33+f->memory[i];
    printf("CHECKSUM %08x copy=%u\n",hash,copy[123]);
    return 0;
}
