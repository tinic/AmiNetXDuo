/*
 * telnet -- the TELNET protocol, RFC 854.
 *
 *     telnet HOST/A,PORT,DEBUG=-d/S,QUIET/S
 *
 * A connection with a command language embedded in it.  Nearly everything a
 * telnet session carries is data, but 0xFF (IAC) introduces two- and
 * three-byte commands that must be taken out of the stream before the user
 * sees them, and answered -- a client that prints them is the one whose
 * screen fills with accented characters the moment it meets a real server.
 *
 *   RFC 854's rule is that an option is only in effect once both ends have
 *   agreed, and that a refusal is a complete and correct answer.  This client
 *   therefore agrees to exactly two options and refuses everything else:
 *
 *     ECHO (1)                 the server may echo for us -- accepted, and
 *                              local echo goes off when it is
 *     SUPPRESS-GO-AHEAD (3)    in both directions; this is what puts a Unix
 *                              server into character-at-a-time mode
 *
 *   TERMINAL-TYPE, NAWS, LINEMODE, environment passing and the rest are
 *   refused with WONT/DONT.  That is not a gap: half-implementing an option
 *   whose subnegotiation we cannot complete is how a session hangs waiting
 *   for a reply that never comes, and a server told WONT simply does without.
 *
 *   The state of every option is remembered, both halves of it, because the
 *   protocol's one real trap is the negotiation loop: an end that answers
 *   every DONT with a WONT and every WONT with a DONT will trade them with a
 *   peer doing the same thing until one of them is switched off.  See the
 *   note above tn_recv_will() for the exact rule used, which is not quite the
 *   one RFC 854 states.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

const char *const tool_name = "telnet";

static const char version_tag[] __attribute__((used)) =
    "$VER: telnet 1.0 (25.7.2026)";

#define TEMPLATE    "HOST/A,PORT,DEBUG=-d/S,QUIET/S"

enum
{
    ARG_HOST = 0,
    ARG_PORT,
    ARG_DEBUG,
    ARG_QUIET,
    ARG_COUNT
};

/* ------------------------------------------------------------- protocol --- */

#define TN_SE       240
#define TN_NOP      241
#define TN_DM       242
#define TN_BRK      243
#define TN_IP       244
#define TN_AO       245
#define TN_AYT      246
#define TN_EC       247
#define TN_EL       248
#define TN_GA       249
#define TN_SB       250
#define TN_WILL     251
#define TN_WONT     252
#define TN_DO       253
#define TN_DONT     254
#define TN_IAC      255

#define TNOPT_BINARY    0
#define TNOPT_ECHO      1
#define TNOPT_SGA       3
#define TNOPT_STATUS    5
#define TNOPT_TTYPE     24
#define TNOPT_NAWS      31
#define TNOPT_TSPEED    32
#define TNOPT_LINEMODE  34
#define TNOPT_NEWENV    39

/* Ctrl-] -- the escape everyone's fingers already know. */
#define TN_ESCAPE       0x1D

#define TN_CHUNK        4096

static UBYTE tn_from_net[TN_CHUNK];
static UBYTE tn_from_user[TN_CHUNK];
static UBYTE tn_staged[TN_CHUNK * 2];   /* IAC and CR both double a byte */

/*
 * One byte bigger than the read, And the one byte is the point.
 *
 * tn_demux() emits at most one byte per byte consumed -- except for the first,
 * because a CR held back from the PREVIOUS segment (st->saw_cr, which survives
 * between calls by design) resolves into a '\n' and then falls through to emit
 * the byte that decided it as well. So a segment that ends on a bare CR
 * followed by a full 4096-byte segment carrying no CR and no IAC produces 4097
 * bytes, and the far end chooses both. That is a server-controlled one-byte
 * write past the end of a static, which on a machine with no MMU lands in
 * whichever static the linker put next.
 *
 * TN_CHUNK + 1 is exact rather than generous: no other path in the parser
 * expands.
 */
static UBYTE tn_clean[TN_CHUNK + 1];

/*
 * The negotiated state, both halves.  "him" is what the far end may do, "us"
 * is what we have promised to do.  TRUE means the option is on.
 */
static UBYTE tn_him[256];
static UBYTE tn_us[256];

/* Where the IAC parser is between recv() calls. */
enum
{
    TN_DATA = 0,
    TN_SAW_IAC,
    TN_SAW_WILL,
    TN_SAW_WONT,
    TN_SAW_DO,
    TN_SAW_DONT,
    TN_SAW_SB,
    TN_SAW_SB_IAC
};

typedef struct TnState
{
    struct Library *sb;
    LONG            sock;
    int             parse;          /* one of TN_DATA..TN_SAW_SB_IAC       */
    BOOL            debug;
    BOOL            local_echo;
    BOOL            saw_cr;         /* the last data byte was a bare CR    */
    BOOL            failed;
} TnState;


/* ---------------------------------------------------------------- names --- */

static const char *tn_option_name(UBYTE opt)
{
    switch (opt)
    {
        case TNOPT_BINARY:   return "BINARY";
        case TNOPT_ECHO:     return "ECHO";
        case TNOPT_SGA:      return "SUPPRESS-GO-AHEAD";
        case TNOPT_STATUS:   return "STATUS";
        case TNOPT_TTYPE:    return "TERMINAL-TYPE";
        case TNOPT_NAWS:     return "WINDOW-SIZE";
        case TNOPT_TSPEED:   return "TERMINAL-SPEED";
        case TNOPT_LINEMODE: return "LINEMODE";
        case TNOPT_NEWENV:   return "ENVIRONMENT";
        default:             return "option";
    }
}

static const char *tn_verb_name(UBYTE verb)
{
    switch (verb)
    {
        case TN_WILL: return "WILL";
        case TN_WONT: return "WONT";
        case TN_DO:   return "DO";
        case TN_DONT: return "DONT";
        default:      return "?";
    }
}


/* --------------------------------------------------------- negotiation --- */

static VOID tn_send_raw(TnState *st, const UBYTE *buf, LONG len)
{
    if (st->failed)
        return;

    if (tool_sock_send(st->sb, st->sock, buf, len) != len)
    {
        tool_error("cannot send: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
        st->failed = TRUE;
    }
}

static VOID tn_reply(TnState *st, UBYTE verb, UBYTE opt)
{
    UBYTE out[3];

    out[0] = TN_IAC;
    out[1] = verb;
    out[2] = opt;

    if (st->debug)
        tool_printf("telnet: sent %s %s (%ld)\n", (LONG)tn_verb_name(verb),
                    (LONG)tn_option_name(opt), (LONG)opt);

    tn_send_raw(st, out, 3);
}

/*
 * The four halves of the negotiation.
 *
 * Two arrays are not enough: alongside "is the option on" there has to be
 * "have we answered a request for it", because RFC 854's loop-prevention rule
 * ("do not reply if the reply would not change the state") taken literally
 * means a client that refuses an option never answers at all, and a server
 * waiting for that answer hangs.  The first request always gets a reply; a
 * repeat of one already answered does not.  That satisfies both halves of the
 * rule, which is the trap this protocol is famous for.
 */
static UBYTE tn_him_told[256];
static UBYTE tn_us_told[256];

/* "The far end says it WILL do <opt>."  ECHO and SUPPRESS-GO-AHEAD cost us
   nothing and are accepted; the rest are refused. */
static VOID tn_recv_will(TnState *st, UBYTE opt)
{
    BOOL accept = (BOOL)(opt == TNOPT_ECHO || opt == TNOPT_SGA);

    if (accept)
    {
        if (tn_him[opt] && tn_him_told[opt])
            return;

        tn_him[opt]      = 1;
        tn_him_told[opt] = 1;
        tn_reply(st, TN_DO, opt);

        /* If the server is echoing, we must not: two echoes is a stutter. */
        if (opt == TNOPT_ECHO)
            st->local_echo = FALSE;
        return;
    }

    if (!tn_him[opt] && tn_him_told[opt])
        return;

    tn_him[opt]      = 0;
    tn_him_told[opt] = 1;
    tn_reply(st, TN_DONT, opt);
}

static VOID tn_recv_wont(TnState *st, UBYTE opt)
{
    if (!tn_him[opt] && tn_him_told[opt])
        return;

    tn_him[opt]      = 0;
    tn_him_told[opt] = 1;
    tn_reply(st, TN_DONT, opt);

    if (opt == TNOPT_ECHO)
        st->local_echo = TRUE;
}

/*
 * "The far end asks us to do <opt>."  SUPPRESS-GO-AHEAD is agreed -- we never
 * send GA anyway, so the promise is free and it is what a Unix server is
 * waiting for before it will run character-at-a-time.  Everything else is
 * refused, including every option whose subnegotiation we could not finish.
 */
static VOID tn_recv_do(TnState *st, UBYTE opt)
{
    BOOL accept = (BOOL)(opt == TNOPT_SGA);

    if (accept)
    {
        if (tn_us[opt] && tn_us_told[opt])
            return;

        tn_us[opt]      = 1;
        tn_us_told[opt] = 1;
        tn_reply(st, TN_WILL, opt);
        return;
    }

    if (!tn_us[opt] && tn_us_told[opt])
        return;

    tn_us[opt]      = 0;
    tn_us_told[opt] = 1;
    tn_reply(st, TN_WONT, opt);
}

static VOID tn_recv_dont(TnState *st, UBYTE opt)
{
    if (!tn_us[opt] && tn_us_told[opt])
        return;

    tn_us[opt]      = 0;
    tn_us_told[opt] = 1;
    tn_reply(st, TN_WONT, opt);
}


/* ------------------------------------------------------------- the flow --- */

/*
 * Take one block off the network: strip the commands, answer them, and leave
 * the data in tn_clean.  Returns the number of data bytes.
 *
 * The parser's state survives between calls, because a three-byte command can
 * and does arrive split across two segments.
 */
static LONG tn_demux(TnState *st, const UBYTE *buf, LONG len)
{
    LONG i;
    LONG out = 0;

    for (i = 0; i < len; i++)
    {
        UBYTE c = buf[i];

        switch (st->parse)
        {
            case TN_DATA:
                if (c == TN_IAC)
                {
                    st->parse = TN_SAW_IAC;
                    break;
                }

                /*
                 * NVT line endings: CR LF is a new line and CR NUL is a bare
                 * carriage return.  The Amiga console wants a single LF for
                 * the first and nothing useful for the second, so the CR is
                 * held back one byte and decided when the next one arrives.
                 */
                if (st->saw_cr)
                {
                    st->saw_cr = FALSE;
                    if (c == '\0')
                    {
                        tn_clean[out++] = '\n';
                        break;
                    }
                    if (c == '\n')
                    {
                        tn_clean[out++] = '\n';
                        break;
                    }
                    tn_clean[out++] = '\n';
                    /* and fall through to emit c as itself */
                }

                if (c == '\r')
                {
                    st->saw_cr = TRUE;
                    break;
                }

                tn_clean[out++] = c;
                break;

            case TN_SAW_IAC:
                if (c == TN_IAC)
                {
                    tn_clean[out++] = TN_IAC;   /* an escaped 0xFF */
                    st->parse = TN_DATA;
                }
                else if (c == TN_WILL) { st->parse = TN_SAW_WILL; }
                else if (c == TN_WONT) { st->parse = TN_SAW_WONT; }
                else if (c == TN_DO)   { st->parse = TN_SAW_DO;   }
                else if (c == TN_DONT) { st->parse = TN_SAW_DONT; }
                else if (c == TN_SB)   { st->parse = TN_SAW_SB;   }
                else
                {
                    /*
                     * NOP, DM, BRK, AYT and the rest.  None of them needs an
                     * answer from a client that has agreed to no options, and
                     * an AYT answered by a program nobody is watching is
                     * noise; they are consumed and dropped.
                     */
                    if (st->debug)
                        tool_printf("telnet: command %ld ignored\n", (LONG)c);
                    st->parse = TN_DATA;
                }
                break;

            case TN_SAW_WILL:
                if (st->debug)
                    tool_printf("telnet: got WILL %s (%ld)\n",
                                (LONG)tn_option_name(c), (LONG)c);
                tn_recv_will(st, c);
                st->parse = TN_DATA;
                break;

            case TN_SAW_WONT:
                if (st->debug)
                    tool_printf("telnet: got WONT %s (%ld)\n",
                                (LONG)tn_option_name(c), (LONG)c);
                tn_recv_wont(st, c);
                st->parse = TN_DATA;
                break;

            case TN_SAW_DO:
                if (st->debug)
                    tool_printf("telnet: got DO %s (%ld)\n",
                                (LONG)tn_option_name(c), (LONG)c);
                tn_recv_do(st, c);
                st->parse = TN_DATA;
                break;

            case TN_SAW_DONT:
                if (st->debug)
                    tool_printf("telnet: got DONT %s (%ld)\n",
                                (LONG)tn_option_name(c), (LONG)c);
                tn_recv_dont(st, c);
                st->parse = TN_DATA;
                break;

            case TN_SAW_SB:
                /*
                 * A subnegotiation, for an option we cannot have agreed to.
                 * Swallow it to IAC SE rather than guess at its length --
                 * this is the one place where a wrong byte count would put
                 * option bytes on the user's screen.
                 */
                if (c == TN_IAC)
                    st->parse = TN_SAW_SB_IAC;
                break;

            case TN_SAW_SB_IAC:
                if (c == TN_SE)
                {
                    if (st->debug)
                        tool_printf("telnet: subnegotiation ignored\n");
                    st->parse = TN_DATA;
                }
                else if (c == TN_IAC)
                {
                    st->parse = TN_SAW_SB;      /* escaped 0xFF inside SB */
                }
                else
                {
                    st->parse = TN_SAW_SB;
                }
                break;

            default:
                st->parse = TN_DATA;
                break;
        }
    }

    return out;
}

/*
 * The user's bytes, on their way out.  Two substitutions, both required:
 * a literal 0xFF has to be doubled or it starts a command, and a line ending
 * has to go out as CR LF, which is what NVT means by "new line" whatever the
 * local convention is.
 *
 * Returns -1 when the escape character was pressed.
 */
static LONG tn_encode(const UBYTE *buf, LONG len, UBYTE *out, BOOL interactive)
{
    LONG i;
    LONG o = 0;

    for (i = 0; i < len; i++)
    {
        UBYTE c = buf[i];

        if (interactive && c == TN_ESCAPE)
            return -1;

        if (c == TN_IAC)
        {
            out[o++] = TN_IAC;
            out[o++] = TN_IAC;
            continue;
        }

        if (c == '\r')
        {
            /* A CR LF pair from a file must not become Cr lf lf. */
            if (i + 1 < len && buf[i + 1] == '\n')
                i++;
            out[o++] = '\r';
            out[o++] = '\n';
            continue;
        }

        if (c == '\n')
        {
            out[o++] = '\r';
            out[o++] = '\n';
            continue;
        }

        out[o++] = c;
    }

    return o;
}


/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    ToolSockAddr    sa;
    ToolInput       in;
    TnState         st;
    const char     *host;
    ULONG           address = 0;
    UWORD           port;
    BOOL            quiet;
    LONG            rc = RETURN_OK;
    ULONG           i;
    char            dotted[16];

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (ULONG)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<host> [<port>]",
                   "Opens a TELNET session.  Ctrl-] closes it.");
        return RETURN_ERROR;
    }

    host  = (const char *)args[ARG_HOST];
    quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    for (i = 0; i < 256UL; i++)
    {
        tn_him[i]      = 0;
        tn_us[i]       = 0;
        tn_him_told[i] = 0;
        tn_us_told[i]  = 0;
    }

    st.sb         = NULL;
    st.sock       = -1;
    st.parse      = TN_DATA;
    st.debug      = (args[ARG_DEBUG] != 0) ? TRUE : FALSE;
    st.local_echo = TRUE;
    st.saw_cr     = FALSE;
    st.failed     = FALSE;

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    st.sb = sb;

    port = (args[ARG_PORT] != 0)
               ? tool_sock_port(sb, (const char *)args[ARG_PORT], "tcp")
               : 23;
    if (port == 0)
    {
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!tool_sock_resolve(sb, host, &address))
    {
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    ami_config_format_ip(address, dotted, sizeof(dotted));

    if (!quiet)
        tool_printf("Trying %s port %ld...\n", (LONG)dotted, (LONG)port);

    st.sock = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    if (st.sock < 0)
    {
        tool_error("no socket: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    tool_sock_addr(&sa, address, port);

    if (tool_sock_connect(sb, st.sock, &sa) != 0)
    {
        tool_sock_fail(sb, "connect to", address, port);
        (VOID)tool_sock_close(sb, st.sock);
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!quiet)
    {
        tool_printf("Connected to %s.\n", (LONG)host);
        tool_printf("Escape character is Ctrl-].\n");
    }

    /*
     * RAW mode, so a keystroke reaches the server the moment it is pressed
     * rather than when the line is finished.  On a redirected input this does
     * nothing and the whole file is shovelled instead, which is what makes a
     * scripted session possible.
     */
    tool_input_open(&in, TRUE);

    for (;;)
    {
        ToolFdSet   readfds;
        ToolTimeval tv;
        LONG        ready;
        LONG        n;

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            rc = RETURN_WARN;
            break;
        }

        if (st.failed)
        {
            rc = RETURN_ERROR;
            break;
        }

        /* ---- keyboard --------------------------------------------------- */

        if (!in.eof)
        {
            n = tool_input_read(&in, tn_from_user, (LONG)sizeof(tn_from_user),
                                0UL);

            if (n > 0)
            {
                LONG len = tn_encode(tn_from_user, n, tn_staged,
                                     in.interactive);

                if (len < 0)
                {
                    if (!quiet)
                        tool_printf("\nConnection closed.\n");
                    break;
                }

                if (st.local_echo && in.interactive && len > 0)
                    (VOID)tool_output_write(tn_from_user, n);

                tn_send_raw(&st, tn_staged, len);
                continue;
            }

            /*
             * End of input is NOT the end of the session, and this is where
             * an earlier version got it wrong.
             *
             * Half-closing here -- shutdown(SHUT_WR), which is right for nc
             * -- breaks telnet, because the negotiation is still going on:
             * the server's WILL ECHO can arrive after the last line of a
             * script, and the DO ECHO that answers it then fails with EPIPE
             * on a write half we have already given away.  Measured: every
             * option reply after the first was lost that way.
             *
             * So a script that has run out simply stops typing.  The session
             * ends when the server closes it, at Ctrl-], or at Ctrl-C.
             */
        }

        /* ---- network ---------------------------------------------------- */

        tool_fd_zero(&readfds);
        tool_fd_add(&readfds, st.sock);

        tv.tv_secs  = 0;
        tv.tv_micro = 50000;

        ready = tool_sock_select(sb, st.sock + 1, &readfds, NULL, &tv);

        if (ready < 0)
        {
            if (tool_sock_errno(sb) == TOOL_EINTR)
                continue;

            tool_error("the connection failed: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            rc = RETURN_ERROR;
            break;
        }

        if (ready == 0)
            continue;

        n = tool_sock_recv(sb, st.sock, tn_from_net, (LONG)sizeof(tn_from_net));

        if (n == 0)
        {
            if (!quiet)
                tool_printf("\nConnection closed by foreign host.\n");
            break;
        }

        if (n < 0)
        {
            LONG err = tool_sock_errno(sb);

            /* Ready, and then nothing there: pace it, or the retry is a spin
               at full CPU.  Same reasoning as nc_shovel(). */
            if (err == TOOL_EINTR || err == TOOL_EWOULDBLOCK)
            {
                (VOID)tool_delay_ticks(2);
                continue;
            }

            tool_error("the connection failed: %s",
                       (LONG)tool_sock_errstr(err));
            rc = RETURN_ERROR;
            break;
        }

        n = tn_demux(&st, tn_from_net, n);

        if (n > 0 && tool_output_write(tn_clean, n) != n)
        {
            tool_fault(IoErr());
            rc = RETURN_FAIL;
            break;
        }
    }

    tool_input_close(&in);

    (VOID)tool_sock_close(sb, st.sock);
    CloseLibrary(sb);
    FreeArgs(rda);

    return (int)rc;
}
