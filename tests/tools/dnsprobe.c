/*
 * DnsProbe, exercises the resolver half of the Roadshow API on a running
 * stack, through the published LVOs and nothing of ours.
 *
 * Three things a build cannot check:
 *
 *   1. AddDomainNameServer() nests.  "Adding the same address twice will
 *      require two calls RemoveDomainNameServer() to remove it again"
 *      (autodoc).  Two programs sharing a name server is the case: if the
 *      first Remove drops the entry, the second program's resolver dies with
 *      the first program.  Only the use count in the list makes that visible,
 *      and only a running stack has one.
 *
 *   2. dnsn_UseCount distinguishes where an entry came from.  Negative is
 *      "configured statically in the file", positive is the real number of
 *      run-time adds.  A stack that reported -1 for everything passes any
 *      test that only checks the sign of the file's own entry, so the probe
 *      adds one of its own and asserts the sign flips.
 *
 *   3. inet_pton() is not inet_addr().  Its INPUTS are "numbers in the range
 *      0..255", decimal, four of them; inet_addr() keeps the 4.3BSD radixes
 *      and short forms.  Both go through the same LVO table to the same
 *      library, so the probe asks each the same strings and prints both
 *      answers side by side, a shared parser shows up as agreement.
 *
 *   4. gethostname() into a buffer that is too small.  "The returned name is
 *      null-terminated unless insufficient space is provided", and the ERRORS
 *      list is EFAULT and EPERM, a short buffer is not a failure at all.
 *      The probe asks byte by byte and prints what came back and whether it
 *      was terminated; only a stack with a real name can answer that.
 *
 *   5. h_name is the OFFICIAL name.  A DEVS:Internet/hosts entry matches on
 *      its aliases too, so asking for an alias must answer with the entry's
 *      own name and list the alias in h_aliases.  The staged hosts file gives
 *      this machine's own address a name and an alias for exactly this, which
 *      also drives gethostname()'s host-database step.
 *
 * The resolver phase needs a real name server.  Under SLIRP that is 10.0.2.3
 * forwarding to the host's, which is what tests/tools/run-dns.sh arranges.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno -0x0a2 */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static ULONG p_inet_addr(struct Library *base, const char *cp)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)cp;
    register ULONG           res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-180:W)"     /* inet_addr -0x0b4 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_inet_pton(struct Library *base, LONG af, const char *src,
                        APTR dst)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = af;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)src;
    register APTR            a1  __asm("a1") = dst;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-606:W)"     /* inet_pton -0x25e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG p_add_dns(struct Library *base, const char *address)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)address;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-516:W)"     /* AddDomainNameServer -0x204 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_remove_dns(struct Library *base, const char *address)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)address;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-522:W)"     /* RemoveDomainNameServer -0x20a */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static struct List *p_obtain_dns_list(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register struct List    *res __asm("d0");

    __asm __volatile ("jsr a6@(-534:W)"     /* ObtainDomainNameServerList -0x216 */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static VOID p_release_dns_list(struct Library *base, struct List *list)
{
    register struct Library *a6 __asm("a6") = base;
    register struct List    *a0 __asm("a0") = list;
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-528:W)"     /* ReleaseDomainNameServerList -0x210 */
                      : "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d0", "d1", "a1", "cc", "memory");
}

/*
 * This one returns BOOL, which is a 16-bit short: the answer is D0.w and the
 * top half of D0 is whatever the callee left there. Reading all 32 bits gets
 * a number like 2621440 for FALSE, so the probe takes the word the ABI
 * defines, as any caller compiled against the published prototype does.
 */
static LONG p_get_domain(struct Library *base, char *buffer, LONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)buffer;
    register LONG            d0  __asm("d0") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-702:W)"     /* GetDefaultDomainName -0x2be */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "a1", "cc", "memory");
    return (LONG)(WORD)res;
}

static VOID p_set_domain(struct Library *base, const char *name)
{
    register struct Library *a6 __asm("a6") = base;
    register CONST_APTR      a0 __asm("a0") = (CONST_APTR)name;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-708:W)"     /* SetDefaultDomainName -0x2c4 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

/* Only the two fields this probe reads; the layout is the published one. */
struct probe_hostent
{
    char   *h_name;
    char  **h_aliases;
    LONG    h_addrtype;
    LONG    h_length;
    char  **h_addr_list;
};

static struct probe_hostent *p_gethostbyname(struct Library *base,
                                             const char *name)
{
    register struct Library      *a6  __asm("a6") = base;
    register CONST_APTR           a0  __asm("a0") = (CONST_APTR)name;
    register struct probe_hostent *res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"     /* gethostbyname -0x0d2 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_gethostname(struct Library *base, char *buffer, LONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)buffer;
    register LONG            d0  __asm("d0") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-282:W)"     /* gethostname -0x11a */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "a1", "cc", "memory");
    return res;
}

/* ----------------------------------------------------------- little helpers */

static VOID p_dotted(ULONG net_addr, char *out)
{
    static const char digits[] = "0123456789";
    const UBYTE      *b        = (const UBYTE *)&net_addr;
    ULONG             pos      = 0;
    ULONG             i;

    for (i = 0; i < 4; i++)
    {
        ULONG v = b[i];

        if (v >= 100)
            out[pos++] = digits[v / 100];
        if (v >= 10)
            out[pos++] = digits[(v / 10) % 10];
        out[pos++] = digits[v % 10];

        if (i != 3)
            out[pos++] = '.';
    }
    out[pos] = '\0';
}

static BOOL p_same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }

    return (BOOL)(*a == '\0' && *b == '\0');
}

/*
 * The use count of one address in the live list, or 0 if it is not there,
 * a slot in use never reports 0, so that is an unambiguous "absent".
 */
static LONG p_dns_use(struct Library *base, const char *address)
{
    struct List *list = p_obtain_dns_list(base);
    LONG         use  = 0;

    if (list != NULL)
    {
        struct MinNode *node = (struct MinNode *)list->lh_Head;

        while (node != NULL && node->mln_Succ != NULL)
        {
            /* struct DomainNameServerNode, as libraries/bsdsocket.h has it. */
            const struct
            {
                struct MinNode  dnsn_MinNode;
                LONG            dnsn_Size;
                char           *dnsn_Address;
                LONG            dnsn_UseCount;
            } *dns = (VOID *)node;

            if (dns->dnsn_Address != NULL &&
                p_same(dns->dnsn_Address, address))
                use = dns->dnsn_UseCount;

            node = node->mln_Succ;
        }

        p_release_dns_list(base, list);
    }

    return use;
}

static VOID p_show_list(struct Library *base, const char *when)
{
    struct List *list = p_obtain_dns_list(base);

    if (list == NULL)
    {
        Printf((CONST_STRPTR)"dnslist %s: NULL\n", (LONG)when);
        return;
    }

    {
        struct MinNode *node = (struct MinNode *)list->lh_Head;

        while (node != NULL && node->mln_Succ != NULL)
        {
            const struct
            {
                struct MinNode  dnsn_MinNode;
                LONG            dnsn_Size;
                char           *dnsn_Address;
                LONG            dnsn_UseCount;
            } *dns = (VOID *)node;

            Printf((CONST_STRPTR)"dnslist %s: %s use %ld size %ld\n",
                   (LONG)when,
                   (LONG)(dns->dnsn_Address != NULL ? dns->dnsn_Address : "?"),
                   dns->dnsn_UseCount, dns->dnsn_Size);

            node = node->mln_Succ;
        }
    }

    p_release_dns_list(base, list);
}

/* --------------------------------------------------------- the pton phase - */

/*
 * Each string goes to both calls. inet_pton() must take only the strict form;
 * inet_addr() must still take everything 4.3BSD took, because that is its own
 * documented behaviour and this stack's own code reads addresses with it.
 */
static VOID p_pton_phase(struct Library *base)
{
    static const char *const cases[] =
    {
        "1.2.3.4",
        "0.0.0.0",
        "255.255.255.255",      /* inet_addr's answer is INADDR_NONE here   */
        "127.0.0.1",
        "0177.0.0.1",           /* octal: inet_addr yes, inet_pton no      */
        "0x1.2.3.4",            /* hex:   inet_addr yes, inet_pton no      */
        "0x7f000001",           /* one hex part                            */
        "010.1.1.1",            /* leading zero                            */
        "1.2.3.04",             /* leading zero, last part                 */
        "127.1",                /* short form                              */
        "1.2.3",
        "1.2.3.4.5",
        "256.1.1.1",
        "1.2.3.4 ",             /* trailing space                          */
        " 1.2.3.4",
        "1.2.3.",
        "",
        NULL
    };
    ULONG i;

    for (i = 0; cases[i] != NULL; i++)
    {
        ULONG dst  = 0xDEADBEEFUL;
        LONG  rc   = p_inet_pton(base, 2L, cases[i], &dst);
        ULONG lax  = p_inet_addr(base, cases[i]);
        char  ptext[16];
        char  atext[16];

        p_dotted(dst, ptext);
        p_dotted(lax, atext);

        Printf((CONST_STRPTR)"pton \"%s\" rc %ld -> %s | addr -> %s\n",
               (LONG)cases[i], rc, (LONG)(rc == 1 ? ptext : "-"),
               (LONG)(lax == 0xFFFFFFFFUL ? "INADDR_NONE" : atext));
    }

    /* Not AF_INET: -1, and distinct from the 0 a malformed address gets. */
    {
        ULONG dst = 0;

        Printf((CONST_STRPTR)"pton family 99 rc %ld\n",
               p_inet_pton(base, 99L, "1.2.3.4", &dst));
    }
}

/* ------------------------------------------------------ the hostname phase - */

static VOID p_lookup(struct Library *base, const char *name, const char *label)
{
    struct probe_hostent *he = p_gethostbyname(base, name);

    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
    {
        Printf((CONST_STRPTR)"%s \"%s\": FAILED\n", (LONG)label, (LONG)name);
        return;
    }

    {
        ULONG addr;
        char  text[16];

        /* h_addr_list[0] is four bytes in network order, not aligned by the
           published interface. */
        addr = ((ULONG)(UBYTE)he->h_addr_list[0][0] << 24) |
               ((ULONG)(UBYTE)he->h_addr_list[0][1] << 16) |
               ((ULONG)(UBYTE)he->h_addr_list[0][2] <<  8) |
                (ULONG)(UBYTE)he->h_addr_list[0][3];

        p_dotted(addr, text);
        Printf((CONST_STRPTR)"%s \"%s\": %s\n", (LONG)label, (LONG)name,
               (LONG)text);
    }
}

/*
 * A truncated name may legitimately come back without a terminator, so the
 * buffer is filled with a sentinel first, only the n bytes the call owns are
 * examined, and byte n is stamped with a NUL afterwards purely so Printf has
 * something it can walk.
 */
static VOID p_hostname_size(struct Library *base, LONG n)
{
    char  buf[64];
    LONG  rc;
    LONG  i;
    BOOL  term = FALSE;

    for (i = 0; i < (LONG)sizeof(buf); i++)
        buf[i] = '#';

    rc = p_gethostname(base, buf, n);

    for (i = 0; i < n; i++)
        if (buf[i] == '\0')
            term = TRUE;

    buf[n] = '\0';

    Printf((CONST_STRPTR)"hostname %ld: rc %ld errno %ld \"%s\" term %s\n",
           n, rc, p_errno(base), (LONG)buf, (LONG)(term ? "yes" : "no"));
}

/* h_name must be the entry's own name however the caller spelled it. */
static VOID p_official(struct Library *base, const char *name)
{
    struct probe_hostent *he = p_gethostbyname(base, name);

    if (he == NULL)
    {
        Printf((CONST_STRPTR)"official \"%s\": FAILED\n", (LONG)name);
        return;
    }

    Printf((CONST_STRPTR)"official \"%s\": name \"%s\" alias \"%s\"\n",
           (LONG)name,
           (LONG)((he->h_name != NULL) ? he->h_name : "?"),
           (LONG)((he->h_aliases != NULL && he->h_aliases[0] != NULL)
                      ? he->h_aliases[0] : ""));
}

static VOID p_hostname_phase(struct Library *base, const char *self_name,
                             const char *self_alias)
{
    char buf[300];
    LONG rc;
    LONG i;

    for (i = 0; i < (LONG)sizeof(buf); i++)
        buf[i] = '#';

    rc = p_gethostname(base, buf, (LONG)sizeof(buf));
    Printf((CONST_STRPTR)"hostname full: rc %ld \"%s\"\n", rc, (LONG)buf);

    /* Not a buffer at all: EFAULT, the one error the autodoc does list. */
    Printf((CONST_STRPTR)"hostname null: rc %ld errno %ld\n",
           p_gethostname(base, NULL, 32L), p_errno(base));

    for (i = 1; i <= 8; i++)
        p_hostname_size(base, i);

    /* The two lengths that matter: exactly the name, where the terminator is
       what has to go, and one more, where it fits. */
    for (i = 0; self_name[i] != '\0'; i++)
        ;
    p_hostname_size(base, i);
    p_hostname_size(base, i + 1);

    /* The alias and the official name of the same entry. */
    p_official(base, self_alias);
    p_official(base, self_name);

    /* INADDR_NONE is a valid broadcast address (autodoc BUGS): a literal, not
       a name to look up. */
    p_lookup(base, "255.255.255.255", "broadcast");
}

/* ------------------------------------------------------- the nesting phase - */

/*
 * 192.0.2.53 is in TEST-NET-1 (RFC 5737): reserved for documentation, routed
 * nowhere, so adding it cannot make this machine query a stranger. It is
 * removed again before the resolver phase either way.
 */
#define PROBE_EXTRA_DNS     "192.0.2.53"

static VOID p_nesting_phase(struct Library *base, const char *file_server)
{
    LONG rc;

    p_show_list(base, "initial");

    Printf((CONST_STRPTR)"static %s use %ld\n",
           (LONG)file_server, p_dns_use(base, file_server));

    /* The file's own entry, added once by a program: still static, deeper. */
    rc = p_add_dns(base, file_server);
    Printf((CONST_STRPTR)"add %s rc %ld use %ld\n", (LONG)file_server, rc,
           p_dns_use(base, file_server));

    /* THE BUG: this used to drop the entry and take the resolver with it. */
    rc = p_remove_dns(base, file_server);
    Printf((CONST_STRPTR)"remove %s rc %ld use %ld\n", (LONG)file_server, rc,
           p_dns_use(base, file_server));

    /* A server this probe owns: positive count, and it goes when it empties. */
    rc = p_add_dns(base, PROBE_EXTRA_DNS);
    Printf((CONST_STRPTR)"add %s rc %ld use %ld\n", (LONG)PROBE_EXTRA_DNS, rc,
           p_dns_use(base, PROBE_EXTRA_DNS));

    rc = p_add_dns(base, PROBE_EXTRA_DNS);
    Printf((CONST_STRPTR)"add %s rc %ld use %ld\n", (LONG)PROBE_EXTRA_DNS, rc,
           p_dns_use(base, PROBE_EXTRA_DNS));

    rc = p_remove_dns(base, PROBE_EXTRA_DNS);
    Printf((CONST_STRPTR)"remove %s rc %ld use %ld\n", (LONG)PROBE_EXTRA_DNS,
           rc, p_dns_use(base, PROBE_EXTRA_DNS));

    rc = p_remove_dns(base, PROBE_EXTRA_DNS);
    Printf((CONST_STRPTR)"remove %s rc %ld use %ld\n", (LONG)PROBE_EXTRA_DNS,
           rc, p_dns_use(base, PROBE_EXTRA_DNS));

    /* One remove too many: -1 and ENOENT, not a silent success. */
    rc = p_remove_dns(base, PROBE_EXTRA_DNS);
    Printf((CONST_STRPTR)"remove %s again rc %ld errno %ld\n",
           (LONG)PROBE_EXTRA_DNS, rc, p_errno(base));

    p_show_list(base, "final");
}

/* -------------------------------------------------- the default-domain phase */

static VOID p_domain_phase(struct Library *base, const char *host,
                           const char *domain)
{
    char buffer[300];
    char original[300];
    LONG ok;

    original[0] = '\0';
    ok = p_get_domain(base, original, (LONG)sizeof(original));
    Printf((CONST_STRPTR)"domain initial: rc %ld \"%s\"\n", ok, (LONG)original);

    /*
     * A 200-character domain. SetDefaultDomainName()'s autodoc allows 255,
     * and the store used to be 64, so this round trip is the whole assertion
     * for that cap.
     */
    {
        char longname[201];
        ULONG i;

        for (i = 0; i < 200; i++)
            longname[i] = (char)((i % 10 == 9) ? '.' : 'a');
        longname[199] = 'z';                    /* never a trailing dot */
        longname[200] = '\0';

        p_set_domain(base, longname);
        buffer[0] = '\0';
        ok = p_get_domain(base, buffer, (LONG)sizeof(buffer));
        Printf((CONST_STRPTR)"domain 200 chars: rc %ld roundtrip %s\n", ok,
               (LONG)(p_same(buffer, longname) ? "yes" : "NO"));
    }

    /* Control: no default domain, so the bare name has nothing to fall back
       on and must fail. Run first, so a pass later cannot be something else
       resolving it. */
    p_set_domain(base, "");
    ok = p_get_domain(base, buffer, (LONG)sizeof(buffer));
    Printf((CONST_STRPTR)"domain cleared: rc %ld\n", ok);
    p_lookup(base, host, "control");

    /* The reference answer, asked for by its full name. */
    {
        char full[128];
        ULONG n = 0, i;

        for (i = 0; host[i] != '\0'; i++)
            full[n++] = host[i];
        full[n++] = '.';
        for (i = 0; domain[i] != '\0'; i++)
            full[n++] = domain[i];
        full[n] = '\0';

        p_lookup(base, full, "qualified");
    }

    p_set_domain(base, domain);
    buffer[0] = '\0';
    ok = p_get_domain(base, buffer, (LONG)sizeof(buffer));
    Printf((CONST_STRPTR)"domain set: rc %ld \"%s\"\n", ok, (LONG)buffer);

    /* The feature: a bare name, resolved through the default domain. */
    p_lookup(base, host, "bare");

    /* A bare name with no answer under the domain either still fails, and
       fails as a lookup failure rather than as something the retry invented. */
    p_lookup(base, "nosuchhost-aminetxduo", "missing");

    p_set_domain(base, original);
}

/* ------------------------------------------------------------------- main - */

int main(int argc, char **argv)
{
    struct Library *base;
    const char     *file_server = "10.0.2.3";
    const char     *host        = "www";
    const char     *domain      = "example.com";
    /* The name and alias run-dns.sh gives this machine's own address in the
       staged DEVS:Internet/hosts. */
    const char     *self_name   = "amiga-probe.localdomain";
    const char     *self_alias  = "amiga-probe";

    if (argc > 1)
        file_server = argv[1];
    if (argc > 2)
        host = argv[2];
    if (argc > 3)
        domain = argv[3];
    if (argc > 4)
        self_name = argv[4];
    if (argc > 5)
        self_alias = argv[5];

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"DnsProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    p_pton_phase(base);
    p_hostname_phase(base, self_name, self_alias);
    p_nesting_phase(base, file_server);
    p_domain_phase(base, host, domain);

    CloseLibrary(base);

    return RETURN_OK;
}
