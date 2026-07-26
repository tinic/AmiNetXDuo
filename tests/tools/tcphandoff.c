/*
 * TcpHandoff -- give a connection to a program that knows nothing about
 * sockets.
 *
 * This is the sequence docs/RESEARCH.md scoped for a telnet server, and the
 * one an SSH server needs too, run end to end with two ordinary AmigaDOS
 * commands standing in for the shell:
 *
 *      listen()/accept()                        this program
 *      ReleaseCopyOfSocket(fd, UNIQUE_ID)       bsdsocket.library, handoff.c
 *      Open("TCP:OBTAIN=<id>")                  the TCP: handler
 *      SystemTagList(cmd, SYS_Output = that)    dos.library
 *
 * The far end of the connection is `Copy TCP:localhost/<port> TO <file>`,
 * started asynchronously before the accept(). The near end, once the socket
 * has become a file handle, is `Echo`. Neither command has any idea it is
 * talking over TCP; between them they prove that a file handle made this way
 * is an ordinary one, in both directions, to programs that were compiled
 * years before any of this existed.
 *
 * Nothing here is linked against our code: the library is reached through its
 * published LVOs, exactly as a third-party program would reach it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: TcpHandoff 1.0 (26.7.2026)";

/* The port this program listens on, and the one the far end dials. */
#define HANDOFF_PORT        2300

#define HANDOFF_MESSAGE \
    "Echo \"handoff payload: a shell command wrote this down a socket\""

#define PEER_COMMAND \
    "SYS:Copy TCP:localhost/2300 TO DH0:handoff.txt"

#define PEER_LOG            "DH0:handoff-peer.txt"

/* bsdsocket.library LVOs, straight out of the NDK's bsdsocket_lib.fd. */
#define LVO_socket              (-30)
#define LVO_bind                (-36)
#define LVO_listen              (-42)
#define LVO_accept              (-48)
#define LVO_setsockopt          (-90)
#define LVO_CloseSocket        (-120)
#define LVO_ReleaseCopyOfSocket (-156)
#define LVO_Errno              (-162)

#define AF_INET_LOCAL           2
#define SOCK_STREAM_LOCAL       1
#define SOL_SOCKET_LOCAL        0xFFFF
#define SO_REUSEADDR_LOCAL      0x0004
#define UNIQUE_ID_LOCAL         (-1)

/* 4.4BSD, which is what this NDK and this library both use. */
struct sockaddr_in_local
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

/* devices/timer.h's struct timeval, spelled out so this file needs no NDK
   headers beyond dos and exec. */
struct TimeVal_local
{
    ULONG   tv_secs;
    ULONG   tv_micro;
};

/* How long to wait for the peer command to reach us before giving up. */
#define ACCEPT_TIMEOUT_SECS 60

static struct Library *SocketBase;

static LONG call_socket(LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG call_bind(LONG s, struct sockaddr_in_local *sa, LONG len)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = len;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "d2", "cc", "memory");
    return res;
}

static LONG call_listen(LONG s, LONG backlog)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = backlog;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "d2", "cc", "memory");
    return res;
}

static LONG call_accept(LONG s)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = NULL;
    register APTR            a1  __asm("a1") = NULL;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "d2", "cc", "memory");
    return res;
}

static LONG call_setsockopt(LONG s, LONG level, LONG name, APTR val, LONG len)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register APTR            a0  __asm("a0") = val;
    register LONG            d3  __asm("d3") = len;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

static LONG call_close_socket(LONG s)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG call_release_copy(LONG s, LONG id)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = id;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-156:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "d2", "cc", "memory");
    return res;
}

/*
 * WaitSelect() with a timeout, so that "the connection never arrived" is a
 * report rather than a hang. It is also the call an Amiga server loop is
 * really built around, so exercising it here is not a detour.
 */
static LONG call_wait_select(LONG nfds, APTR rd, struct TimeVal_local *tv)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = rd;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = tv;
    register ULONG          *d1  __asm("d1") = NULL;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "d2", "cc", "memory");
    return res;
}

static LONG call_errno(void)
{
    register struct Library *a6  __asm("a6") = SocketBase;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* ------------------------------------------------------------------------- */

/*
 * Progress, written with the file opened and closed around every line.
 *
 * Not Printf(): this program's stdout is a handle the Shell holds open for
 * its whole life, so anything printed is still sitting in a buffer if the
 * program stops early -- and "stopped early" is precisely the case a trace is
 * for. Same reasoning as ToolsSmoke's report().
 */
#define STEP_FILE   "DH0:handoff-steps.txt"

static void step(const char *what, LONG a)
{
    BPTR fh = Open((CONST_STRPTR)STEP_FILE, MODE_READWRITE);
    LONG args[1];

    if (fh == (BPTR)0)
        return;

    Seek(fh, 0, OFFSET_END);
    args[0] = a;
    VFPrintf(fh, (CONST_STRPTR)what, (APTR)args);
    Close(fh);
}

/* "TCP:OBTAIN=<id>", built by hand: no stdio in a command this small. */
static void format_obtain(char *out, LONG id)
{
    char  digits[12];
    ULONG value = (ULONG)id;
    int   n     = 0;
    int   i     = 0;

    for (i = 0; "TCP:OBTAIN="[i] != '\0'; i++)
        out[i] = "TCP:OBTAIN="[i];

    if (value == 0)
        digits[n++] = '0';

    while (value != 0)
    {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (n > 0)
        out[i++] = digits[--n];

    out[i] = '\0';
}

static LONG start_peer(void)
{
    struct TagItem tags[5];
    BPTR           nil = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR           log = Open((CONST_STRPTR)PEER_LOG, MODE_NEWFILE);
    LONG           rc;

    /*
     * SYS_Asynch closes both handles itself when the command finishes, which
     * is why they are opened here and never closed here. They must also be
     * two different handles -- dos.library says so explicitly.
     */
    tags[0].ti_Tag  = SYS_Input;
    tags[0].ti_Data = (ULONG)nil;
    tags[1].ti_Tag  = SYS_Output;
    tags[1].ti_Data = (ULONG)log;
    tags[2].ti_Tag  = SYS_Asynch;
    tags[2].ti_Data = TRUE;
    tags[3].ti_Tag  = SYS_UserShell;
    tags[3].ti_Data = TRUE;
    tags[4].ti_Tag  = TAG_DONE;
    tags[4].ti_Data = 0;

    rc = SystemTagList((CONST_STRPTR)PEER_COMMAND, tags);
    if (rc == -1)
    {
        if (nil) Close(nil);
        if (log) Close(log);
    }

    return rc;
}

static LONG hand_over(LONG client)
{
    struct TagItem tags[4];
    char           name[64];
    LONG           id;
    BPTR           fh;
    BPTR           nil;
    LONG           rc;

    id = call_release_copy(client, UNIQUE_ID_LOCAL);
    if (id < 0)
    {
        Printf((CONST_STRPTR)"TcpHandoff: ReleaseCopyOfSocket failed, errno %ld\n",
               call_errno());
        return RETURN_FAIL;
    }

    Printf((CONST_STRPTR)"TcpHandoff: socket parked under id %ld\n", id);
    step((const char *)"parked under id %ld\n", id);

    format_obtain(name, id);

    fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        Printf((CONST_STRPTR)"TcpHandoff: Open(\"%s\") failed, IoErr %ld\n",
               (LONG)name, IoErr());
        return RETURN_FAIL;
    }

    Printf((CONST_STRPTR)"TcpHandoff: \"%s\" is now a file handle\n", (LONG)name);
    step((const char *)"TCP:OBTAIN= opened\n", 0);

    nil = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);

    tags[0].ti_Tag  = SYS_Input;
    tags[0].ti_Data = (ULONG)nil;
    tags[1].ti_Tag  = SYS_Output;
    tags[1].ti_Data = (ULONG)fh;
    tags[2].ti_Tag  = SYS_UserShell;
    tags[2].ti_Data = TRUE;
    tags[3].ti_Tag  = TAG_DONE;
    tags[3].ti_Data = 0;

    /* Synchronous: System() does NOT close these, so they are ours to close. */
    rc = SystemTagList((CONST_STRPTR)HANDOFF_MESSAGE, tags);

    Printf((CONST_STRPTR)"TcpHandoff: the command returned %ld\n", rc);
    step((const char *)"the handed-over command returned %ld\n", rc);

    Close(fh);
    step((const char *)"the file handle is closed\n", 0);
    if (nil)
        Close(nil);

    return (rc == -1) ? RETURN_FAIL : RETURN_OK;
}

int main(void)
{
    struct sockaddr_in_local sa;
    LONG                     listener, client;
    LONG                     on = 1;
    LONG                     rc = RETURN_FAIL;
    ULONG                    i;

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        Printf((CONST_STRPTR)"TcpHandoff: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    listener = call_socket(AF_INET_LOCAL, SOCK_STREAM_LOCAL, 0);
    if (listener < 0)
    {
        Printf((CONST_STRPTR)"TcpHandoff: socket() failed, errno %ld\n", call_errno());
        goto out;
    }

    (void)call_setsockopt(listener, SOL_SOCKET_LOCAL, SO_REUSEADDR_LOCAL,
                          &on, sizeof(on));

    for (i = 0; i < sizeof(sa); i++)
        ((UBYTE *)&sa)[i] = 0;

    sa.sin_len    = sizeof(sa);
    sa.sin_family = AF_INET_LOCAL;
    sa.sin_port   = HANDOFF_PORT;       /* host order == network order here */
    sa.sin_addr   = 0;                  /* INADDR_ANY */

    if (call_bind(listener, &sa, sizeof(sa)) < 0 ||
        call_listen(listener, 1) < 0)
    {
        Printf((CONST_STRPTR)"TcpHandoff: cannot listen on %ld, errno %ld\n",
               (LONG)HANDOFF_PORT, call_errno());
        call_close_socket(listener);
        goto out;
    }

    Printf((CONST_STRPTR)"TcpHandoff: listening on %ld\n", (LONG)HANDOFF_PORT);
    step((const char *)"listening on %ld\n", (LONG)HANDOFF_PORT);

    if (start_peer() == -1)
    {
        Printf((CONST_STRPTR)"TcpHandoff: could not start the peer command\n");
        step((const char *)"the peer command would not start\n", 0);
        call_close_socket(listener);
        goto out;
    }

    step((const char *)"the peer command is running\n", 0);

    {
        struct TimeVal_local tv;
        ULONG                readset[8];
        LONG                 ready;

        for (i = 0; i < 8; i++)
            readset[i] = 0;
        readset[(ULONG)listener / 32] = 1UL << ((ULONG)listener % 32);

        tv.tv_secs  = ACCEPT_TIMEOUT_SECS;
        tv.tv_micro = 0;

        ready = call_wait_select(listener + 1, readset, &tv);
        step((const char *)"WaitSelect on the listener returned %ld\n", ready);

        if (ready <= 0)
        {
            Printf((CONST_STRPTR)
                   "TcpHandoff: nothing connected within %ld s\n",
                   (LONG)ACCEPT_TIMEOUT_SECS);
            call_close_socket(listener);
            goto out;
        }
    }

    client = call_accept(listener);
    if (client < 0)
    {
        Printf((CONST_STRPTR)"TcpHandoff: accept() failed, errno %ld\n", call_errno());
        call_close_socket(listener);
        goto out;
    }

    Printf((CONST_STRPTR)"TcpHandoff: accepted a connection\n");
    step((const char *)"accepted a connection\n", 0);

    rc = hand_over(client);

    /*
     * The handle taken through TCP:OBTAIN held its own reference; this is the
     * last one, and dropping it is what sends the FIN the peer needs to see
     * end of file.
     */
    call_close_socket(client);
    step((const char *)"the accepted socket is closed\n", 0);

    call_close_socket(listener);
    step((const char *)"the listener is closed\n", 0);

    /* Let the asynchronous peer finish writing its file. */
    Delay(150);

out:
    CloseLibrary(SocketBase);
    step((const char *)"bsdsocket.library is closed\n", 0);

    return (int)rc;
}
