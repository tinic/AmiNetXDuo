/* Real loaded full library through public LVOs; no linked stack/backend or
 * fixture entropy. Boardless loopback only, not physical/add-on coverage.
 * SPDX-License-Identifier: MIT */
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <string.h>
#include "aminetxduo/health.h"
static unsigned passed;
static void say(const char *s)
{ (void)Write(Output(),(APTR)s,(LONG)strlen(s));(void)Flush(Output()); }
#define CHECK(x) do {if (!(x)) {say("research_exec_library=FAIL " #x "\n");return 20;}} while (0)
#define CASE(s) do {passed++;say("research_exec_library=CASE_PASS " s "\n");} while (0)
static LONG sock(struct Library *b,LONG family,LONG type)
{
    LONG result=LP3(0x1e,LONG,sock,LONG,family,d0,LONG,type,d1,LONG,0,d2,,b);
    if (result<0) {
        LONG error=LP0(0xa2,LONG,errno,,b);
        char number[16];unsigned n=0;ULONG value=(ULONG)error;
        do {number[n++]=(char)('0'+value%10);value/=10;} while(value);
        say("research_exec_library=SOCKET_ERRNO ");
        while(n) {(void)Write(Output(),&number[--n],1);}
        say("\n");
    }
    return result;
}
static LONG bindfd(struct Library *b,LONG fd,APTR addr,LONG length)
{ return LP3(0x24,LONG,bindfd,LONG,fd,d0,APTR,addr,a0,LONG,length,d1,,b); }
static LONG closefd(struct Library *b,LONG fd)
{ return LP1(0x78,LONG,closefd,LONG,fd,d0,,b); }
static LONG sendtofd(struct Library *b,LONG fd,APTR bytes,LONG length,APTR addr,LONG alen)
{ return LP6(0x3c,LONG,sendtofd,LONG,fd,d0,APTR,bytes,a0,LONG,length,d1,LONG,0,d2,APTR,addr,a1,LONG,alen,d3,,b); }
static LONG recvfd(struct Library *b,LONG fd,APTR bytes,LONG length)
{ return LP4(0x4e,LONG,recvfd,LONG,fd,d0,APTR,bytes,a0,LONG,length,d1,LONG,0,d2,,b); }
static LONG sendfd(struct Library *b,LONG fd,APTR bytes,LONG length)
{ return LP4(0x42,LONG,sendfd,LONG,fd,d0,APTR,bytes,a0,LONG,length,d1,LONG,0,d2,,b); }
static LONG timeoutfd(struct Library *b,LONG fd)
{
    struct timeval timeout;
    memset(&timeout,0,sizeof(timeout));timeout.tv_sec=2;
    return LP5(0x5a,LONG,timeoutfd,LONG,fd,d0,LONG,SOL_SOCKET,d1,LONG,SO_RCVTIMEO,d2,APTR,&timeout,a0,LONG,sizeof(timeout),d3,,b);
}
static LONG listenfd(struct Library *b,LONG fd)
{ return LP2(0x2a,LONG,listenfd,LONG,fd,d0,LONG,1,d1,,b); }
static LONG connectfd(struct Library *b,LONG fd,APTR addr,LONG length)
{ return LP3(0x36,LONG,connectfd,LONG,fd,d0,APTR,addr,a0,LONG,length,d1,,b); }
static LONG acceptfd(struct Library *b,LONG fd)
{ return LP3(0x30,LONG,acceptfd,LONG,fd,d0,APTR,NULL,a0,APTR,NULL,a1,,b); }
/* IPv4 uses the historical BSD sockaddr; IPv6 follows the published ANXD ABI. */
struct address4 {UBYTE len,family;UWORD port;ULONG address;UBYTE padding[8];};
struct address6 {UBYTE family,pad;UWORD port;ULONG flow;UBYTE address[16];ULONG scope;};
int main(void)
{
    struct Library *a,*b,*master;
    char payload[]="real library loopback",received[64];
    struct address4 v4={16,AF_INET,45123,0x7f000001UL,{0}};
    struct address6 v6={AF_INET6,0,45124,0,{0},0};
    LONG fd,listener,client,accepted;
    ULONG signals=FindTask(NULL)->tc_SigAlloc;
    v6.address[15]=1;
    say("research_exec_library=START\n");
    for (unsigned cycle=0;cycle<2;cycle++)
    {
        a=OpenLibrary((STRPTR)"bsdsocket.library",4);CHECK(a);
        CASE("actual-OpenLibrary-starts-full-profile-loopback");
        b=OpenLibrary((STRPTR)"bsdsocket.library",4);CHECK(b && a!=b);
        CASE("second-opener-has-private-base");
        fd=sock(a,AF_INET,SOCK_DGRAM);CHECK(fd>=0 && timeoutfd(a,fd)==0);
        CHECK(bindfd(a,fd,&v4,sizeof(v4))==0);
        CHECK(sendtofd(a,fd,payload,sizeof(payload),&v4,sizeof(v4))==sizeof(payload));
        memset(received,0,sizeof(received));
        CHECK(recvfd(a,fd,received,sizeof(received))==sizeof(payload) && !memcmp(payload,received,sizeof(payload)));
        CHECK(closefd(a,fd)==0);CASE("IPv4-UDP-real-send-and-receive-through-LVOs");
        fd=sock(b,AF_INET6,SOCK_DGRAM);CHECK(fd>=0 && timeoutfd(b,fd)==0);
        CHECK(bindfd(b,fd,&v6,sizeof(v6))==0);
        CHECK(sendtofd(b,fd,payload,sizeof(payload),&v6,sizeof(v6))==sizeof(payload));
        memset(received,0,sizeof(received));
        CHECK(recvfd(b,fd,received,sizeof(received))==sizeof(payload) && !memcmp(payload,received,sizeof(payload)));
        CHECK(closefd(b,fd)==0);CASE("IPv6-UDP-real-send-and-receive-through-LVOs");
        v4.port=(UWORD)(45125+cycle);
        listener=sock(a,AF_INET,SOCK_STREAM);client=sock(b,AF_INET,SOCK_STREAM);
        CHECK(listener>=0 && client>=0 && bindfd(a,listener,&v4,sizeof(v4))==0 && listenfd(a,listener)==0);
        CHECK(connectfd(b,client,&v4,sizeof(v4))==0);
        accepted=acceptfd(a,listener);CHECK(accepted>=0 && timeoutfd(a,accepted)==0);
        CHECK(sendfd(b,client,payload,sizeof(payload))==sizeof(payload));
        memset(received,0,sizeof(received));
        CHECK(recvfd(a,accepted,received,sizeof(received))==sizeof(payload) && !memcmp(payload,received,sizeof(payload)));
        CHECK(closefd(a,accepted)==0 && closefd(b,client)==0 && closefd(a,listener)==0);
        CASE("IPv4-TCP-connect-accept-and-data-between-private-bases");
        /* Leave B cached and A evicted: closing A must reclaim its signal
         * debt without requiring a later call through that base. */
        fd=sock(b,AF_INET,SOCK_DGRAM);CHECK(fd>=0 && closefd(b,fd)==0);
        CloseLibrary(a);
        fd=sock(b,AF_INET,SOCK_DGRAM);CHECK(fd>=0 && closefd(b,fd)==0);
        CASE("first-close-preserves-second-opener");
        Forbid();master=(struct Library *)FindName(&SysBase->LibList,(STRPTR)"bsdsocket.library");Permit();
        CHECK(master);
        say("research_exec_library=LAST_CLOSE\n");CloseLibrary(b);
        CHECK(master->lib_OpenCnt==0);
        CHECK(!FindSemaphore((STRPTR)AMI_HEALTH_NAME));
        CHECK(!FindTask((STRPTR)"Exec NetX management"));
        CHECK(!FindTask((STRPTR)"Exec NetX clock"));
        CHECK(!FindTask((STRPTR)"AmiNetXDuo ip"));
        CASE("last-close-retires-actual-helper-kernel-and-published-health");
        /* Expunge returns the seglist; the test explicitly unloads it. */
        Forbid();APTR segment=LP0(0x12,APTR,expunge,,master);Permit();
        CHECK(segment);UnLoadSeg((BPTR)segment);
        Forbid();master=(struct Library *)FindName(&SysBase->LibList,(STRPTR)"bsdsocket.library");Permit();
        CHECK(!master);CASE("actual-expunge-removes-loaded-library-before-next-cycle");
    }
    CHECK(passed==16 && FindTask(NULL)->tc_SigAlloc==signals);
    say("research_exec_library=PASS 16/16 actual_loaded_cycles=2\n");return 0;
}
