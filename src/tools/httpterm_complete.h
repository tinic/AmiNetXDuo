/*
 * Tab completion for the web Shell, the part with no DOS in it.
 *
 * The Shell edits its command line in the browser (LINE mode), so the far
 * side never sees a half-typed word.  On Tab the page sends `complete <c>
 * <token>` -- c is 1 at the command position, 0 elsewhere -- and httpterm.c
 * answers `comp <name>\n<name>\n...`, the matching names, sorted.  The page
 * replaces the word with the first and cycles through the rest on further
 * Tabs, the way PowerShell does.  Resolving the token against the Shell's
 * current directory (and, at the command position, every directory C: is
 * assigned to) is DOS and lives in httpterm.c.
 *
 * What lives here is everything BUT the directory read: splitting the token
 * into a directory and a prefix, matching a name against the prefix the
 * AmigaDOS way (case-insensitive), and collecting the matches into a sorted,
 * de-duplicated list.  It compiles and runs on the host, so what the page is
 * handed is tested there rather than only on the A3000.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPTERM_COMPLETE_H
#define AMINETXDUO_HTTPTERM_COMPLETE_H

#include <stddef.h>

#define TERM_COMP_PATHMAX   256     /* a token, split into dir + prefix       */
#define TERM_COMP_LISTMAX  1200     /* total bytes of the names kept          */
#define TERM_COMP_MAXN       48     /* how many names before the rest is cut  */
#define TERM_COMP_NAMEMAX   120     /* the longest single name kept           */

typedef struct
{
    char           tc_buf[TERM_COMP_LISTMAX];   /* the names, each NUL-ended   */
    unsigned short tc_off[TERM_COMP_MAXN];       /* where each starts in tc_buf */
    unsigned char  tc_dir[TERM_COMP_MAXN];       /* it is a directory           */
    unsigned short tc_len;                        /* bytes used in tc_buf        */
    unsigned short tc_count;                      /* names collected             */
} TermCompletion;

/* AmigaDOS lower-case: ASCII plus the Latin-1 accented letters, the same fold
   the file systems do, so a name and a prefix differing only in case match. */
static unsigned char term_comp_lower(unsigned char c)
{
    if (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        return (unsigned char)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)     /* À..Þ except × */
        return (unsigned char)(c + 32);
    return c;
}

/* Case-insensitive compare, for sorting and de-duplicating. */
static int term_comp_cmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        unsigned char la = term_comp_lower((unsigned char)*a);
        unsigned char lb = term_comp_lower((unsigned char)*b);

        if (la != lb)
            return (int)la - (int)lb;
        a++;
        b++;
    }
    return (int)term_comp_lower((unsigned char)*a) -
           (int)term_comp_lower((unsigned char)*b);
}

/* Does `name` begin with `prefix`, case-insensitively?  An empty prefix
   matches everything -- Tab on nothing lists the directory. */
static int term_comp_match(const char *name, const char *prefix)
{
    size_t i;

    for (i = 0; prefix[i] != '\0'; i++)
    {
        if (name[i] == '\0')
            return 0;
        if (term_comp_lower((unsigned char)name[i]) !=
            term_comp_lower((unsigned char)prefix[i]))
            return 0;
    }

    return 1;
}

static void term_comp_init(TermCompletion *c)
{
    c->tc_len   = 0;
    c->tc_count = 0;
}

static int term_comp_has(const TermCompletion *c, const char *name)
{
    unsigned short i;

    for (i = 0; i < c->tc_count; i++)
        if (term_comp_cmp(c->tc_buf + c->tc_off[i], name) == 0)
            return 1;

    return 0;
}

/* Keep one matching entry.  The caller has already checked term_comp_match.
   Duplicates -- the same command from two of C:'s drawers -- are dropped, and
   the list stops growing at MAXN names or LISTMAX bytes. */
static void term_comp_add(TermCompletion *c, const char *name, int is_dir)
{
    size_t nl = 0;
    size_t i;

    while (name[nl] != '\0')
        nl++;

    if (nl == 0 || nl > TERM_COMP_NAMEMAX)
        return;
    if (c->tc_count >= TERM_COMP_MAXN)
        return;
    if ((size_t)c->tc_len + nl + 1 > TERM_COMP_LISTMAX)
        return;
    if (term_comp_has(c, name))
        return;

    c->tc_off[c->tc_count] = c->tc_len;
    c->tc_dir[c->tc_count] = (unsigned char)(is_dir ? 1 : 0);
    for (i = 0; i < nl; i++)
        c->tc_buf[c->tc_len++] = name[i];
    c->tc_buf[c->tc_len++] = '\0';
    c->tc_count++;
}

/* Sort the collected names case-insensitively, so the page cycles through
   them in a predictable order.  Insertion sort: the list is short. */
static void term_comp_sort(TermCompletion *c)
{
    unsigned short i;

    for (i = 1; i < c->tc_count; i++)
    {
        unsigned short off = c->tc_off[i];
        unsigned char  dir = c->tc_dir[i];
        unsigned short j   = i;

        while (j > 0 &&
               term_comp_cmp(c->tc_buf + c->tc_off[j - 1],
                             c->tc_buf + off) > 0)
        {
            c->tc_off[j] = c->tc_off[j - 1];
            c->tc_dir[j] = c->tc_dir[j - 1];
            j--;
        }
        c->tc_off[j] = off;
        c->tc_dir[j] = dir;
    }
}

/* Write the names into out, one per line, a '/' after a directory so the next
   Tab can descend into it.  Returns the length written. */
static size_t term_comp_emit(const TermCompletion *c, char *out, size_t size)
{
    size_t         o = 0;
    unsigned short i;

    if (size == 0)
        return 0;

    for (i = 0; i < c->tc_count; i++)
    {
        const char *nm = c->tc_buf + c->tc_off[i];
        size_t      k;

        for (k = 0; nm[k] != '\0' && o < size - 2; k++)
            out[o++] = nm[k];
        if (c->tc_dir[i] && o < size - 2)
            out[o++] = '/';
        if (o < size - 1)
            out[o++] = '\n';
    }

    out[o] = '\0';
    return o;
}

/*
 * Split a token into the directory to read and the prefix to match in it.
 *
 *   "mkl"            -> dir ""         prefix "mkl"   (the current directory)
 *   "busybox/mkl"    -> dir "busybox"  prefix "mkl"
 *   "Work:mkl"       -> dir "Work:"    prefix "mkl"
 *   "Work:sub/mkl"   -> dir "Work:sub" prefix "mkl"
 *   "Work:"          -> dir "Work:"    prefix ""
 *   "busybox/"       -> dir "busybox"  prefix ""
 *
 * The ':' stays with the directory (Lock() needs "Work:"); the last '/' is the
 * separator and is dropped.
 */
static void term_comp_split(const char *token, char *dir, size_t dsize,
                            char *prefix, size_t psize)
{
    size_t len = 0;
    long   cut = -1;    /* index of the last '/' or ':'                       */
    int    colon = 0;   /* the cut is a ':'                                   */
    size_t i;
    size_t start;
    size_t o;

    while (token[len] != '\0')
        len++;

    for (i = 0; i < len; i++)
    {
        if (token[i] == '/')  { cut = (long)i; colon = 0; }
        else if (token[i] == ':') { cut = (long)i; colon = 1; }
    }

    if (cut < 0)
    {
        dir[0] = '\0';
        start  = 0;
    }
    else
    {
        size_t dlen = colon ? (size_t)cut + 1 : (size_t)cut;   /* keep ':'    */

        o = 0;
        for (i = 0; i < dlen && o < dsize - 1; i++)
            dir[o++] = token[i];
        dir[o] = '\0';

        start = (size_t)cut + 1;
    }

    o = 0;
    for (i = start; i < len && o < psize - 1; i++)
        prefix[o++] = token[i];
    prefix[o] = '\0';
}

#endif /* AMINETXDUO_HTTPTERM_COMPLETE_H */
