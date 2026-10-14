/*
 * arp FILE: one `<host> <ether_addr> [TEMP] [PUB]` per line.  The record
 * rules in plain C, so the host test runs what arp runs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ARPFILE_H
#define AMINETXDUO_ARPFILE_H

#define ARPFILE_OK       0      /* *host, *mac, *temp set                     */
#define ARPFILE_SKIP     1      /* blank, or a # or ; comment                 */
#define ARPFILE_NOMAC    2      /* a host and nothing after it                */
#define ARPFILE_PUB      3      /* PUB/PUBLISH: proxy ARP is not supported    */
#define ARPFILE_WORD     4      /* *bad is neither TEMP nor PUB               */

/*
 * A chunk FGets() returned is a whole record when it ends in a newline, or
 * when it is shorter than the buffer allowed (the last line of a file with no
 * newline).  Anything else is the first part of a longer line.
 */
static inline int arpfile_partial(const char *chunk, unsigned long max)
{
    unsigned long n = 0;

    while (chunk[n] != '\0')
        n++;

    return n >= max && chunk[n - 1] != '\n';
}

static inline int arpfile_same(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++)
    {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;

        if (ca != *b)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static inline char *arpfile_word(char **cursor)
{
    char *p = *cursor;
    char *word;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0' || *p == '\n' || *p == '\r')
        return 0;

    word = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    if (*p != '\0')
        *p++ = '\0';

    *cursor = p;
    return word;
}

/*
 * The rest of an overlong line.  `get` returns the next character, -1 at the
 * end of the file, -2 on a read error.  ARPFILE_DRAINED at the newline or the
 * end, ARPFILE_READERR on an error: the caller stops there, because what is
 * left cannot be told apart from the line it belongs to.
 */
#define ARPFILE_DRAINED  0
#define ARPFILE_READERR  (-1)

static inline int arpfile_drain(long (*get)(void *), void *ctx)
{
    for (;;)
    {
        long c = get(ctx);

        if (c == -2)
            return ARPFILE_READERR;
        if (c == -1 || c == '\n')
            return ARPFILE_DRAINED;
    }
}

/* One whole record, split in place. */
static inline int arpfile_parse(char *line, char **host, char **mac,
                                int *temp, char **bad)
{
    char *cursor = line;
    char *word;

    *temp = 0;
    *host = arpfile_word(&cursor);
    if (*host == 0 || (*host)[0] == '#' || (*host)[0] == ';')
        return ARPFILE_SKIP;

    *mac = arpfile_word(&cursor);
    if (*mac == 0)
        return ARPFILE_NOMAC;

    while ((word = arpfile_word(&cursor)) != 0)
    {
        if (arpfile_same(word, "TEMP"))
            *temp = 1;
        else if (arpfile_same(word, "PUB") || arpfile_same(word, "PUBLISH"))
            return ARPFILE_PUB;
        else
        {
            *bad = word;
            return ARPFILE_WORD;
        }
    }

    return ARPFILE_OK;
}

#endif /* AMINETXDUO_ARPFILE_H */
