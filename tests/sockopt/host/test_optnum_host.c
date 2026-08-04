/*
 * Every option number this library answers to, against the published one.
 *
 * The IPv6 socket options have two numberings in the wild and this NDK
 * publishes neither, so bsdsocket.library accepts the 4.4BSD/KAME numbers and,
 * for a few options, the Linux ones as well.  An alias is only safe while
 * nothing else claims that number, and twice now one has not been:
 *
 *   26  Linux IPV6_V6ONLY, BSD IPV6_CHECKSUM, withdrawn on a raw socket
 *   50  Linux IPV6_PKTINFO, BSD IPV6_DSTOPTS, the alias set removed
 *
 * Neither is visible from the outside.  setsockopt returns success, getsockopt
 * answers, and a caller who spelled the BSD number has its buffer read as
 * something else entirely.  So this reads the numbers out of the tree and
 * checks each against the BSD/KAME numbering, which is the lineage the rest of
 * the header set belongs to; a new alias that lands on a taken number fails
 * here rather than in a caller.
 *
 * Host-side because it is a fact about numbers, and because it then runs on
 * every build rather than only where an emulator does.  It parses the headers
 * rather than including them: they need <exec/types.h>, and a table restated
 * here would only ever agree with itself.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- the BSD numbering,
 *
 * netinet6/in6.h as KAME wrote it and the BSDs ship it, plus the RFC 3542
 * additions.  This is the external fact the tree is checked against; only the
 * options a caller could plausibly reach are listed, which is all of them from
 * 1 to 62 that have a name.
 */
typedef struct
{
    int         number;
    const char *name;
} OptName;

static const OptName bsd_ipv6[] =
{
    {  1, "IPV6_SOCKOPT_RESERVED1" },
    {  4, "IPV6_UNICAST_HOPS"      },
    {  9, "IPV6_MULTICAST_IF"      },
    { 10, "IPV6_MULTICAST_HOPS"    },
    { 11, "IPV6_MULTICAST_LOOP"    },
    { 12, "IPV6_JOIN_GROUP"        },
    { 13, "IPV6_LEAVE_GROUP"       },
    { 24, "IPV6_PORTRANGE"         },
    { 26, "IPV6_CHECKSUM"          },
    { 27, "IPV6_V6ONLY"            },
    { 36, "IPV6_RECVPKTINFO"       },
    { 37, "IPV6_RECVHOPLIMIT"      },
    { 38, "IPV6_RECVRTHDR"         },
    { 39, "IPV6_RECVHOPOPTS"       },
    { 40, "IPV6_RECVDSTOPTS"       },
    { 42, "IPV6_USE_MIN_MTU"       },
    { 43, "IPV6_RECVPATHMTU"       },
    { 44, "IPV6_PATHMTU"           },
    { 46, "IPV6_PKTINFO"           },
    { 47, "IPV6_HOPLIMIT"          },
    { 48, "IPV6_NEXTHOP"           },
    { 49, "IPV6_HOPOPTS"           },
    { 50, "IPV6_DSTOPTS"           },
    { 51, "IPV6_RTHDR"             },
    { 52, "IPV6_PKTOPTIONS"        },
    { 53, "IPV6_RTHDRDSTOPTS"      },
    { 57, "IPV6_RECVTCLASS"        },
    { 59, "IPV6_AUTOFLOWLABEL"     },
    { 61, "IPV6_TCLASS"            },
    { 62, "IPV6_DONTFRAG"          }
};

/*
 * The one alias that stays, and why it is allowed to sit on a taken number.
 * 26 is Linux's IPV6_V6ONLY and BSD's IPV6_CHECKSUM, which is a raw-socket
 * option; bsd_v6_linux_numbering() in in6.c withdraws the Linux numbering on a
 * raw socket, so the two never both apply.  Anything else that collides has
 * not been decided about and fails.
 */
typedef struct
{
    int         number;
    const char *ours;
    const char *why;
} Exemption;

static const Exemption exempt[] =
{
    { 26, "AMI_IPV6_V6ONLY_LINUX",
      "IPV6_CHECKSUM is raw-only and in6.c withdraws the Linux numbering there" }
};

/* --------------------------------------------------------------- the tree -- */

/*
 * What the tree claims, and what each claim is treated as.  The number is read
 * from the source; the name beside it is what bsdsocket.library does with that
 * number, which is the thing being checked against the BSD meaning.
 */
typedef struct
{
    const char *file;
    const char *define;     /* the #define to find                        */
    const char *treated_as; /* the BSD option this number is handled as   */
} Claim;

static const Claim claims[] =
{
    /* aminetxduo/in6.h, the published numbers, which are BSD's. */
    { "include/aminetxduo/in6.h",  "IPV6_V6ONLY",          "IPV6_V6ONLY"       },
    { "include/aminetxduo/in6.h",  "IPV6_UNICAST_HOPS",    "IPV6_UNICAST_HOPS" },
    { "include/aminetxduo/in6.h",  "IPV6_TCLASS",          "IPV6_TCLASS"       },
    { "include/aminetxduo/in6.h",  "IPV6_MULTICAST_IF",    "IPV6_MULTICAST_IF" },
    { "include/aminetxduo/in6.h",  "IPV6_MULTICAST_HOPS",  "IPV6_MULTICAST_HOPS" },
    { "include/aminetxduo/in6.h",  "IPV6_MULTICAST_LOOP",  "IPV6_MULTICAST_LOOP" },
    { "include/aminetxduo/in6.h",  "IPV6_JOIN_GROUP",      "IPV6_JOIN_GROUP"   },
    { "include/aminetxduo/in6.h",  "IPV6_LEAVE_GROUP",     "IPV6_LEAVE_GROUP"  },

    /* aminetxduo/cmsg.h, RFC 3542's ancillary options, also BSD's numbers. */
    { "include/aminetxduo/cmsg.h", "IPV6_RECVPKTINFO",     "IPV6_RECVPKTINFO"  },
    { "include/aminetxduo/cmsg.h", "IPV6_PKTINFO",         "IPV6_PKTINFO"      },
    { "include/aminetxduo/cmsg.h", "IPV6_RECVHOPLIMIT",    "IPV6_RECVHOPLIMIT" },
    { "include/aminetxduo/cmsg.h", "IPV6_HOPLIMIT",        "IPV6_HOPLIMIT"     },

    /*
     * The Linux aliases setsockopt() also matches, which live in
     * bsdsocket_internal.h because accepting a second numbering is behaviour
     * rather than ABI.  These are the ones that can collide.
     */
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_V6ONLY_LINUX",
      "IPV6_V6ONLY"         },
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_UNICAST_HOPS_LINUX",
      "IPV6_UNICAST_HOPS"   },
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_TCLASS_LINUX",
      "IPV6_TCLASS"         },
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_MULTICAST_IF_LINUX",
      "IPV6_MULTICAST_IF"   },
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_MULTICAST_HOPS_LINUX",
      "IPV6_MULTICAST_HOPS" },
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_MULTICAST_LOOP_LINUX",
      "IPV6_MULTICAST_LOOP" },
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_JOIN_GROUP_LINUX",
      "IPV6_JOIN_GROUP"     },
    { "src/bsdsocket/bsdsocket_internal.h", "AMI_IPV6_LEAVE_GROUP_LINUX",
      "IPV6_LEAVE_GROUP"    }
};

/* ------------------------------------------------------------- the reading -- */

static int failures;
static int checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (ok)
    {
        printf("  ok   %s\n", what);
    }
    else
    {
        failures++;
        printf("  FAIL %s -- %s\n", what, detail);
    }
}

/*
 * The value of `#define <name> <number>` in a file, or -1.  A #define whose
 * body is another name (IPV6_ADD_MEMBERSHIP) is not a number and is skipped by
 * the caller rather than resolved: nothing in the table above is one.
 */
static int define_value(const char *root, const char *file, const char *name)
{
    char  path[1024];
    char  line[1024];
    FILE *fp;
    int   value = -1;
    size_t namelen = strlen(name);

    snprintf(path, sizeof(path), "%s/%s", root, file);

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return -2;
    }

    while (fgets(line, (int)sizeof(line), fp) != NULL)
    {
        const char *p = line;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '#')
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "define", 6) != 0)
            continue;
        p += 6;
        if (*p != ' ' && *p != '\t')
            continue;
        while (*p == ' ' || *p == '\t')
            p++;

        if (strncmp(p, name, namelen) != 0)
            continue;
        p += namelen;
        if (*p != ' ' && *p != '\t')
            continue;               /* a longer name with this prefix */

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p < '0' || *p > '9')
            continue;               /* an alias of another name */

        value = (int)strtol(p, NULL, 10);
        break;
    }

    fclose(fp);

    return value;
}

static const char *bsd_name_of(int number)
{
    size_t i;

    for (i = 0; i < sizeof(bsd_ipv6) / sizeof(bsd_ipv6[0]); i++)
    {
        if (bsd_ipv6[i].number == number)
            return bsd_ipv6[i].name;
    }

    return NULL;
}

static const Exemption *exemption_of(int number, const char *define)
{
    size_t i;

    for (i = 0; i < sizeof(exempt) / sizeof(exempt[0]); i++)
    {
        if (exempt[i].number == number &&
            strcmp(exempt[i].ours, define) == 0)
            return &exempt[i];
    }

    return NULL;
}

int main(void)
{
    const char *root = AMINETXDUO_SOURCE_DIR;
    size_t      i;

    printf("bsdsocket -- IPv6 option numbers against the BSD numbering\n");

    for (i = 0; i < sizeof(claims) / sizeof(claims[0]); i++)
    {
        const Claim *c = &claims[i];
        char         what[256];
        char         detail[512];
        int          number = define_value(root, c->file, c->define);
        const char  *bsd;

        snprintf(what, sizeof(what), "%s", c->define);

        if (number < 0)
        {
            snprintf(detail, sizeof(detail),
                     "no numeric #define in %s -- renamed, or now an alias",
                     c->file);
            check(0, what, detail);
            continue;
        }

        bsd = bsd_name_of(number);

        if (bsd == NULL)
        {
            /* Unclaimed in the BSD numbering: nothing to collide with. */
            snprintf(what, sizeof(what), "%s = %d is unclaimed in BSD",
                     c->define, number);
            check(1, what, "");
            continue;
        }

        if (strcmp(bsd, c->treated_as) == 0)
        {
            snprintf(what, sizeof(what), "%s = %d is BSD's %s",
                     c->define, number, bsd);
            check(1, what, "");
            continue;
        }

        {
            const Exemption *e = exemption_of(number, c->define);

            snprintf(what, sizeof(what), "%s = %d collides with BSD's %s",
                     c->define, number, bsd);

            if (e != NULL)
            {
                printf("  ok   %s -- allowed: %s\n", what, e->why);
                checks++;
                continue;
            }

            snprintf(detail, sizeof(detail),
                     "handled as %s, so a caller spelling %s is misread",
                     c->treated_as, bsd);
            check(0, what, detail);
        }
    }

    /* No two claims may be the same number unless they mean the same option. */
    for (i = 0; i < sizeof(claims) / sizeof(claims[0]); i++)
    {
        size_t j;
        int    a = define_value(root, claims[i].file, claims[i].define);

        if (a < 0)
            continue;

        for (j = i + 1; j < sizeof(claims) / sizeof(claims[0]); j++)
        {
            int  b = define_value(root, claims[j].file, claims[j].define);
            char what[256];
            char detail[512];

            if (b != a)
                continue;
            if (strcmp(claims[i].treated_as, claims[j].treated_as) == 0)
                continue;

            snprintf(what, sizeof(what), "%s and %s are both %d",
                     claims[i].define, claims[j].define, a);
            snprintf(detail, sizeof(detail),
                     "one is handled as %s and the other as %s",
                     claims[i].treated_as, claims[j].treated_as);
            check(0, what, detail);
        }
    }

    printf("\n%d checks, %d failures -- %s\n", checks, failures,
           (failures == 0) ? "PASS" : "FAIL");

    return (failures == 0) ? 0 : 1;
}
