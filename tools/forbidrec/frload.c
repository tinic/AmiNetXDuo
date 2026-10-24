/*
 * frload: the Amiga half of forbidrec's workload.
 *
 *   frload MODE=dns|udp|connect|sock|arp SECS=n [HOST=a.b.c.d] [PORT=n]
 *          [NET=a.b.c.] [FIRST=n] [COUNT=n]
 *
 *   dns      gethostbyname() of a name nobody has asked for before
 *   udp      a new socket per datagram, so a new source port, to HOST at
 *            PORT..PORT+COUNT-1; waits up to 200 ms for the echo
 *   connect  TCP connect() to HOST:PORT and close, over and over
 *   sock     socket()/bind()/listen()/CloseSocket(), TCP and UDP in turn
 *   arp      one datagram each to NET.FIRST .. NET.FIRST+COUNT-1, port 9
 *
 * One key=value line at the end; exit 0 when anything succeeded.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>

/* TO=file: under `Run >file` from httpd's shell, stdout reaches nothing. */
static FILE *out;
#define printf(...) fprintf(out ? out : stdout, __VA_ARGS__)
#include <string.h>

struct Library *SocketBase;

static ULONG ip4(const char *s)
{
    ULONG v = 0, part = 0;
    
    for (; *s; s++)
    {
        if (*s == '.') { v = (v << 8) | part; part = 0; }
        else if (*s >= '0' && *s <= '9') part = part * 10 + (*s - '0');
        else break;
    }
    return (v << 8) | part;
}

static void sin_set(struct sockaddr_in *a, ULONG ip, UWORD port)
{
    memset(a, 0, sizeof(*a));
    a->sin_len         = sizeof(*a);
    a->sin_family      = AF_INET;
    a->sin_port        = port;
    a->sin_addr.s_addr = ip;
}

int main(void)
{
    static const char template[] = "MODE/A,SECS/N,HOST/K,PORT/N,NET/K,FIRST/N,COUNT/N,TO/K";
    LONG           args[8] = { 0 };
    struct RDArgs *rd = ReadArgs((CONST_STRPTR)template, args, NULL);
    const char    *mode;
    ULONG          secs = 30, port = 9, first = 200, count = 50, host = 0, net = 0;
    ULONG          ok = 0, fail = 0, replies = 0, n = 0;
    struct DateStamp d0, d1;
    LONG           el;
    char           name[64];

    if (!rd) { printf("frload result=bad_args template=\"%s\"\n", template); return 20; }
    if (args[7] && !(out = fopen((const char *)args[7], "w")))
    {
        FreeArgs(rd);
        return 20;
    }
    mode = (const char *)args[0];
    if (args[1]) secs  = *(ULONG *)args[1];
    if (args[2]) host  = ip4((const char *)args[2]);
    if (args[3]) port  = *(ULONG *)args[3];
    if (args[4]) net   = ip4((const char *)args[4]);
    if (args[5]) first = *(ULONG *)args[5];
    if (args[6]) count = *(ULONG *)args[6];
    if (!net) net = ip4("192.168.1.0");
    if (!count) count = 1;

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) { printf("frload result=no_bsdsocket\n"); FreeArgs(rd); return 20; }

    DateStamp(&d0);
    for (;;)
    {
        DateStamp(&d1);
        el = (d1.ds_Days - d0.ds_Days) * 86400L + (d1.ds_Minute - d0.ds_Minute) * 60L +
             (d1.ds_Tick - d0.ds_Tick) / 50L;
        if (el >= (LONG)secs || (SetSignal(0, 0) & SIGBREAKF_CTRL_C))
            break;
        n++;

        if (mode[0] == 'd')
        {
            sprintf(name, "fr%08lx%lu.example.com", (ULONG)FindTask(NULL) ^ d1.ds_Tick, n);
            if (gethostbyname((STRPTR)name)) ok++; else fail++;
        }
        else if (mode[0] == 'u')
        {
            struct sockaddr_in a;
            LONG               s = socket(AF_INET, SOCK_DGRAM, 0);
            char               buf[64];

            if (s < 0) { fail++; continue; }
            sin_set(&a, host, (UWORD)(port + n % count));
            if (sendto(s, (APTR)"forbidrec", 9, 0, (struct sockaddr *)&a, sizeof(a)) == 9)
            {
                fd_set         r;
                struct timeval tv;

                tv.tv_sec  = 0;
                tv.tv_usec = 200000;

                ok++;
                FD_ZERO(&r);
                FD_SET(s, &r);
                if (WaitSelect(s + 1, &r, NULL, NULL, &tv, NULL) > 0 &&
                    recv(s, buf, sizeof(buf), 0) > 0)
                    replies++;
            }
            else
                fail++;
            CloseSocket(s);
        }
        else if (mode[0] == 'c')
        {
            struct sockaddr_in a;
            LONG               s = socket(AF_INET, SOCK_STREAM, 0);

            if (s < 0) { fail++; continue; }
            sin_set(&a, host, (UWORD)port);
            if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) ok++; else fail++;
            CloseSocket(s);
        }
        else if (mode[0] == 's')
        {
            struct sockaddr_in a;
            LONG               s = socket(AF_INET, (n & 1) ? SOCK_DGRAM : SOCK_STREAM, 0);

            if (s < 0) { fail++; continue; }
            sin_set(&a, 0, 0);
            if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 &&
                ((n & 1) || listen(s, 4) == 0))
                ok++;
            else
                fail++;
            CloseSocket(s);
        }
        else if (mode[0] == 'a')
        {
            static LONG        s = -1;
            struct sockaddr_in a;

            if (s < 0 && (s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { fail++; continue; }
            sin_set(&a, net | ((first + n % count) & 0xff), 9);
            if (sendto(s, (APTR)"forbidrec", 9, 0, (struct sockaddr *)&a, sizeof(a)) == 9)
                ok++;
            else
                fail++;
            Delay(1);
        }
        else
        {
            printf("frload result=bad_mode mode=%s\n", mode);
            break;
        }
    }
    printf("frload mode=%s secs=%ld ops=%lu ok=%lu fail=%lu replies=%lu per_s=%lu\n", mode,
           el, n, ok, fail, replies, el ? (ok + fail) / (ULONG)el : 0);
    CloseLibrary(SocketBase);
    FreeArgs(rd);
    if (out)
        fclose(out);
    return ok ? 0 : 10;
}
