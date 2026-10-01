/* Exercise the actual event loop with ready pipes instead of physical inputs. */
#define main frontend_main
#include "../FreeRDP-old-master/l300/fbfreerdp.c"
#undef main
#include <assert.h>
static int network_fd, key_count, dispatches;
static int ready_fds(rdpInst *inst,void **r,int *nr,void **w,int *nw)
{ r[(*nr)++]=(void *)(long)network_fd; return 0; }
static int key(rdpInst *inst,RD_BOOL release,RD_BOOL ext,uint8 code)
{ key_count++; return 0; }
static int network_dispatch(rdpInst *inst)
{
    assert(key_count==64); /* One bounded 32-event read from each device first. */
    dispatches++; running=0; return 0;
}
int main(void)
{
    Frontend *f=calloc(1,sizeof(*f)); rdpSet s; rdpInst *inst;
    int net[2],input[2][2],i,j; struct input_event ev; struct timeval start,end;
    defaults(&s); freerdp_global_init(); inst=freerdp_new(&s); inst->param1=f; f->timeout=5;
    assert(!pipe(net)); network_fd=net[0]; assert(write(net[1],"x",1)==1);
    inst->rdp_get_fds=ready_fds; inst->rdp_check_fds=network_dispatch;
    inst->rdp_send_input_scancode=key;
    for(i=0;i<2;i++) {
        assert(!pipe(input[i])); f->input[f->ninput++]=input[i][0];
        fcntl(input[i][0],F_SETFL,O_NONBLOCK);
        memset(&ev,0,sizeof(ev)); ev.type=EV_KEY; ev.code=KEY_A; ev.value=1;
        for(j=0;j<40;j++) assert(write(input[i][1],&ev,sizeof(ev))==sizeof(ev));
    }
    gettimeofday(&start,NULL); assert(event_loop(inst)==0); gettimeofday(&end,NULL);
    assert(dispatches==1 && key_count==64);
    assert((end.tv_sec-start.tv_sec)*1000000+end.tv_usec-start.tv_usec<100000);
    puts("PASS ready select wakes immediately, input precedes graphics, bounded input batches let graphics run");
    for(i=0;i<2;i++) { close(input[i][0]); close(input[i][1]); }
    f->ninput=1; assert(!pipe(input[0])); f->input[0]=input[0][0];
    ev.code=KEY_LEFTCTRL; assert(write(input[0][1],&ev,sizeof(ev))==sizeof(ev));
    ev.code=KEY_LEFTALT; assert(write(input[0][1],&ev,sizeof(ev))==sizeof(ev));
    ev.code=KEY_F12; assert(write(input[0][1],&ev,sizeof(ev))==sizeof(ev));
    running=1; assert(event_loop(inst)==0); assert(f->local_exit && dispatches==1);
    puts("PASS Ctrl+Alt+F12 exits before another graphics dispatch");
    return 0;
}
