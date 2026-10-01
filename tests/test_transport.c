/* Real TLS 1.2 records containing multiple RDP Fast-Path Synchronize PDUs. */
#define main frontend_main
#include "../FreeRDP-old-master/l300/fbfreerdp.c"
#undef main
#include "rdp.h"
#include "network.h"
#include <assert.h>
static int updates;
static void counted_end(rdpInst *inst) { end_update(inst); updates++; }
int main(int argc,char **argv)
{
    rdpSet s; rdpInst *inst; rdpRdp *rdp; Frontend *f=calloc(1,sizeof(*f));
    struct timeval tv,start,end; fd_set rfds; int fd,ret;
    assert(argc==2); defaults(&s); freerdp_global_init(); inst=freerdp_new(&s);
    assert(fb_open(f,NULL,1)==0); callbacks(inst,f); inst->ui_end_update=counted_end;
    rdp=inst->rdp;
    assert(tcp_connect(rdp->net->tcp,"127.0.0.1",atoi(argv[1])));
    fd=rdp->net->tcp->sockfd; rdp->net->tls=tls_new();
    assert(tls_connect(rdp->net->tls,fd)); rdp->net->tls_connected=1;
    assert(tcp_can_recv(fd,2000)); assert(inst->rdp_check_fds(inst)==0); assert(updates==1);
    assert(!tcp_can_recv(fd,0)); assert(tls_pending(rdp->net->tls)==5);
    assert(freerdp_has_pending_data(inst));
    puts("PASS socket empty while OpenSSL holds another complete 5-byte RDP PDU");
    FD_ZERO(&rfds); FD_SET(fd,&rfds);
    tv.tv_sec=freerdp_has_pending_data(inst)?0:1; tv.tv_usec=0;
    gettimeofday(&start,NULL); ret=select(fd+1,&rfds,NULL,NULL,&tv); gettimeofday(&end,NULL);
    assert(ret==0 && (end.tv_sec-start.tv_sec)*1000000+end.tv_usec-start.tv_usec<100000);
    assert(inst->rdp_check_fds(inst)==0); assert(updates==2);
    assert(!freerdp_has_pending_data(inst));
    puts("PASS second PDU dispatched immediately without any new socket traffic; idle returns to blocking select");
    return 0;
}
