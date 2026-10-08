/* Same public ABI command for original and Exec full LTO libraries.
 * Boardless emulator experiment; no linked backend/NetX or fixture entropy.
 * All clocks are guest E-clock ticks; no output within measured loops.
 * SPDX-License-Identifier: MIT */
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <inline/macros.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <string.h>
#include "aminetxduo/asm_abi.h"
#include "aminetxduo/health.h"
struct Device *TimerBase;
extern AMIGA_ASM_ARGS VOID cal_empty(ULONG reps);
extern AMIGA_ASM_ARGS VOID cal_add(ULONG reps);
#define CALLS 8192UL
#define WARM 32UL
#define SAMPLES 128UL
#define BULK (1024UL*1024UL)
static ULONG rate,passed,latency[SAMPLES];
static UBYTE payload[1024];
static UBYTE bulk_received[BULK];
static struct MsgPort *peer_port;
static struct Message ready_msg,data_msg,done_msg;
static volatile LONG peer_result;
static struct Task *peer_task;
static ULONG peer_release;
static LONG peer_rcv,peer_snd;
static const char peer_name[]="Exec ThreadX benchmark peer";
static void say(const char *s) {(void)Write(Output(),(APTR)s,(LONG)strlen(s));(void)Flush(Output());}
static void number(ULONG n) {char buf[16];unsigned i=0;do{buf[i++]=(char)('0'+n%10);n/=10;}while(n);while(i)(void)Write(Output(),&buf[--i],1);}
static void field(const char *s,ULONG n) {say(s);number(n);}
#define CHECK(x) do{if(!(x)){say("research_exec_bench=FAIL " #x "\n");return 20;}}while(0)
#define CASE(s) do{passed++;say("research_exec_bench=CASE_PASS " s "\n");}while(0)
static ULONG now(void) {struct EClockVal v={0,0};(void)ReadEClock(&v);return v.ev_lo;}
static void metric(const char *name,ULONG ticks,ULONG ops)
{say("BENCH ");say(name);field(" ticks=",ticks);field(" ops=",ops);field(" hz=",rate);say("\n");}
static void memory(const char *name)
{say("RAM ");say(name);field(" available=",AvailMem(MEMF_ANY));field(" largest=",AvailMem(MEMF_ANY|MEMF_LARGEST));say("\n");}
static LONG sock(struct Library *b,LONG type) {return LP3(0x1e,LONG,sock,LONG,AF_INET,d0,LONG,type,d1,LONG,0,d2,,b);}
static LONG closefd(struct Library *b,LONG fd) {return LP1(0x78,LONG,closefd,LONG,fd,d0,,b);}
static LONG bindfd(struct Library *b,LONG fd,APTR a,LONG len) {return LP3(0x24,LONG,bindfd,LONG,fd,d0,APTR,a,a0,LONG,len,d1,,b);}
static LONG listenfd(struct Library *b,LONG fd) {return LP2(0x2a,LONG,listenfd,LONG,fd,d0,LONG,1,d1,,b);}
static LONG connectfd(struct Library *b,LONG fd,APTR a,LONG len) {return LP3(0x36,LONG,connectfd,LONG,fd,d0,APTR,a,a0,LONG,len,d1,,b);}
static LONG acceptfd(struct Library *b,LONG fd) {return LP3(0x30,LONG,acceptfd,LONG,fd,d0,APTR,NULL,a0,APTR,NULL,a1,,b);}
static LONG available(struct Library *b,LONG fd,LONG *n) {return LP3(0x72,LONG,available,LONG,fd,d0,ULONG,FIONREAD,d1,APTR,n,a0,,b);}
static LONG timeoutfd(struct Library *b,LONG fd,LONG option)
{struct timeval t;memset(&t,0,sizeof(t));t.tv_sec=10;return LP5(0x5a,LONG,timeoutfd,LONG,fd,d0,LONG,SOL_SOCKET,d1,LONG,option,d2,APTR,&t,a0,LONG,sizeof(t),d3,,b);}
static LONG option(struct Library *b,LONG fd,LONG name,LONG *value)
{LONG len=sizeof(*value);return LP5(0x60,LONG,option,LONG,fd,d0,LONG,SOL_SOCKET,d1,LONG,name,d2,APTR,value,a0,APTR,&len,a1,,b);}
static LONG waitread(struct Library *b,LONG fd)
{ULONG set=1UL<<fd;struct timeval t;memset(&t,0,sizeof(t));t.tv_sec=10;return LP6(0x7e,LONG,waitread,LONG,fd+1,d0,APTR,&set,a0,APTR,NULL,a1,APTR,NULL,a2,APTR,&t,a3,APTR,NULL,d1,,b);}
static LONG transfer(struct Library *b,LONG fd,UBYTE *buf,ULONG count,int write)
{ULONG total=0;while(total<count){LONG n;if(write)n=LP4(0x42,LONG,send,LONG,fd,d0,APTR,buf+total,a0,LONG,count-total,d1,LONG,0,d2,,b);else n=LP4(0x4e,LONG,recv,LONG,fd,d0,APTR,buf+total,a0,LONG,count-total,d1,LONG,0,d2,,b);if(n<=0)return -1;total+=(ULONG)n;}return (LONG)total;}
struct address4 {UBYTE len,family;UWORD port;ULONG addr;UBYTE pad[8];};
static void peer(void)
{
    struct Library *b=OpenLibrary((STRPTR)"bsdsocket.library",4);
    struct address4 address={16,AF_INET,45180,0x7f000001UL,{0}};
    UBYTE buf[64];LONG listener=-1,fd=-1;ULONG total=0;BYTE release_bit=AllocSignal(-1);
    peer_result=20;
    peer_task=FindTask(NULL);peer_release=release_bit>=0?1UL<<release_bit:0;
    if(b && peer_release){listener=sock(b,SOCK_STREAM);if(listener>=0 && listener<32 && bindfd(b,listener,&address,sizeof(address))==0 && listenfd(b,listener)==0)peer_result=0;}
    PutMsg(peer_port,&ready_msg);
    if(peer_result)goto out;
    if(waitread(b,listener)!=1){peer_result=20;goto out;}
    fd=acceptfd(b,listener);peer_result=20;
    if(fd<0 || timeoutfd(b,fd,SO_RCVTIMEO) || timeoutfd(b,fd,SO_SNDTIMEO) || option(b,fd,SO_RCVBUF,&peer_rcv) || option(b,fd,SO_SNDBUF,&peer_snd))goto out;
    for(ULONG i=0;i<WARM+SAMPLES;i++){
        if(transfer(b,fd,buf,64,0)!=64 || transfer(b,fd,buf,64,1)!=64)goto out;
    }
    while(total<BULK){
        ULONG room=BULK-total;if(room>sizeof(payload))room=sizeof(payload);
        LONG n=LP4(0x4e,LONG,recv,LONG,fd,d0,APTR,bulk_received+total,a0,LONG,room,d1,LONG,0,d2,,b);
        if(n<=0 || (ULONG)n>BULK-total)goto out;
        total+=(ULONG)n;
    }
    peer_result=0;PutMsg(peer_port,&data_msg);
    (void)Wait(peer_release); /* Parent stops timer/verifies before peer teardown. */
out:
    if(fd>=0 && closefd(b,fd))peer_result=20;
    if(listener>=0 && closefd(b,listener))peer_result=20;
    if(b)CloseLibrary(b);
    if(release_bit>=0)FreeSignal(release_bit);
    PutMsg(peer_port,&done_msg);
}
static struct Message *message(void)
{struct Message *m;while(!(m=GetMsg(peer_port)))(void)WaitPort(peer_port);return m;}
static int tcp_run(struct Library *b)
{
    struct TagItem tags[]={{NP_Entry,(ULONG)peer},{NP_Name,(ULONG)peer_name},{NP_StackSize,16384},{NP_Priority,0},{TAG_DONE,0}};
    struct address4 address={16,AF_INET,45180,0x7f000001UL,{0}};
    struct Process *process;LONG fd=-1;int ok=0,done_seen=0,data_seen=0;ULONG start,bulk_ticks=0;UBYTE reply[64];struct Message *m;
    memset(&ready_msg,0,sizeof(ready_msg));memset(&data_msg,0,sizeof(data_msg));memset(&done_msg,0,sizeof(done_msg));
    process=CreateNewProc(tags);if(!process)return 20;
    m=message();done_seen=m==&done_msg;
    if(m!=&ready_msg || peer_result)goto join;
    fd=sock(b,SOCK_STREAM);
    if(fd<0 || timeoutfd(b,fd,SO_RCVTIMEO) || timeoutfd(b,fd,SO_SNDTIMEO) || connectfd(b,fd,&address,sizeof(address)))goto join;
    for(ULONG i=0;i<WARM+SAMPLES;i++){
        if(i==WARM){
            LONG rcv,snd;if(option(b,fd,SO_RCVBUF,&rcv) || option(b,fd,SO_SNDBUF,&snd))goto join;
            say("WINDOW");field(" parent_rcv=",(ULONG)rcv);field(" parent_snd=",(ULONG)snd);field(" peer_rcv=",(ULONG)peer_rcv);field(" peer_snd=",(ULONG)peer_snd);say("\n");memory("connected");
        }
        start=now();
        if(transfer(b,fd,payload,64,1)!=64 || transfer(b,fd,reply,64,0)!=64)goto join;
        ULONG ticks=now()-start;
        if(memcmp(payload,reply,64))goto join;
        if(i>=WARM)latency[i-WARM]=ticks;
    }
    start=now();
    for(ULONG i=0;i<BULK/sizeof(payload);i++)if(transfer(b,fd,payload,sizeof(payload),1)!=sizeof(payload))goto join;
    m=message();done_seen=m==&done_msg;data_seen=m==&data_msg;
    if(m!=&data_msg || peer_result)goto join;
    bulk_ticks=now()-start;ok=1;
    for(ULONG i=0;i<BULK;i++)if(bulk_received[i]!=(UBYTE)i){ok=0;break;}
join:
    if(data_seen)Signal(peer_task,peer_release);
    if(fd>=0 && closefd(b,fd))ok=0;
    /* A failed server may have delivered done where data was expected. */
    Forbid();BOOL alive=FindTask((STRPTR)peer_name)!=NULL;Permit();
    if(alive && !done_seen){do{m=message();}while(m!=&done_msg);}
    else while(GetMsg(peer_port)){}
    for(ULONG i=0;i<100;i++){Forbid();alive=FindTask((STRPTR)peer_name)!=NULL;Permit();if(!alive)break;Delay(1);}
    if(alive || peer_result || !ok)return 20;
    for(ULONG i=1;i<SAMPLES;i++){ULONG x=latency[i],j=i;while(j && latency[j-1]>x){latency[j]=latency[j-1];j--;}latency[j]=x;}
    say("BENCH tcp64");field(" p50_ticks=",latency[SAMPLES/2]);field(" p95_ticks=",latency[(SAMPLES*95)/100]);field(" min_ticks=",latency[0]);field(" max_ticks=",latency[SAMPLES-1]);field(" samples=",SAMPLES);field(" hz=",rate);say("\n");
    CASE("separate-Task-TCP64-request-reply-payload-verified");
    metric("tcp_bulk",bulk_ticks,BULK);CASE("separate-Task-TCP-1MiB-drained-and-verified");return 0;
}
int main(void)
{
    ULONG signals=FindTask(NULL)->tc_SigAlloc,start,empty,add;
    struct MsgPort *timer_port=CreateMsgPort();CHECK(timer_port);
    struct timerequest *io=(struct timerequest *)CreateIORequest(timer_port,sizeof(*io));CHECK(io);
    CHECK(OpenDevice((STRPTR)TIMERNAME,UNIT_MICROHZ,(struct IORequest *)io,0)==0);TimerBase=io->tr_node.io_Device;
    struct EClockVal clock={0,0};rate=ReadEClock(&clock);CHECK(rate);
    start=now();cal_empty(100000);empty=now()-start;start=now();cal_add(100000);add=now()-start;CHECK(add>empty);
    say("BENCH calibration");field(" empty_ticks=",empty);field(" add_ticks=",add);field(" body_ticks=",add-empty);field(" ops=",1600000);field(" hz=",rate);say("\n");CASE("guest-clock-and-existing-CPU-kernels-calibrated");
    memory("before_open");
    struct Library *a=OpenLibrary((STRPTR)"bsdsocket.library",4),*b=OpenLibrary((STRPTR)"bsdsocket.library",4);CHECK(a && b && a!=b);
    memory("opened_two_bases");
    start=now();LONG fa=sock(a,SOCK_DGRAM);ULONG cold=now()-start;CHECK(fa>=0);metric("first_socket",cold,1);
    LONG fb=sock(b,SOCK_DGRAM),n;CHECK(fb>=0);
    memory("two_sockets_adopted");
    for(ULONG i=0;i<256;i++)CHECK(!available(a,fa,&n) && n==0);
    start=now();for(ULONG i=0;i<CALLS;i++)CHECK(!available(a,fa,&n) && n==0);ULONG ticks=now()-start;CHECK(ticks);metric("cached_fionread",ticks,CALLS);CASE("cached-public-bracket-empty-UDP-queue");
    for(ULONG i=0;i<256;i++)CHECK(!available(i&1?b:a,i&1?fb:fa,&n) && n==0);
    start=now();for(ULONG i=0;i<CALLS;i++)CHECK(!available(i&1?b:a,i&1?fb:fa,&n) && n==0);ticks=now()-start;CHECK(ticks);metric("switch_fionread",ticks,CALLS);CASE("same-Task-private-base-switch-empty-UDP-queues");
    CHECK(!closefd(a,fa) && !closefd(b,fb));CloseLibrary(b);
    for(ULONG i=0;i<sizeof(payload);i++)payload[i]=(UBYTE)i;
    peer_port=CreateMsgPort();CHECK(peer_port);CHECK(tcp_run(a)==0);DeleteMsgPort(peer_port);
    CloseLibrary(a);
    CHECK(!FindSemaphore((STRPTR)AMI_HEALTH_NAME));
    Forbid();struct Library *master=(struct Library *)FindName(&SysBase->LibList,(STRPTR)"bsdsocket.library");Permit();CHECK(master && master->lib_OpenCnt==0);
    Forbid();APTR seg=LP0(0x12,APTR,expunge,,master);Permit();CHECK(seg);UnLoadSeg((BPTR)seg);
    memory("unloaded");
    CloseDevice((struct IORequest *)io);TimerBase=NULL;DeleteIORequest((struct IORequest *)io);DeleteMsgPort(timer_port);
    CHECK(FindTask(NULL)->tc_SigAlloc==signals);CASE("actual-peer-exit-library-unload-and-signal-recovery");
    CHECK(passed==6);say("research_exec_bench=PASS 6/6\n");return 0;
}
