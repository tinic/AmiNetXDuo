/*
 * AddNetInterface started from Workbench: the two pieces of it that are
 * plain C, so the host test runs what the command runs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ADDIF_WB_H
#define AMINETXDUO_ADDIF_WB_H

/*
 * A TIMEOUT tool type, read whole: an optional sign and decimal digits, and
 * nothing after them.  1 with *out set, 0 for anything else.  The sign is
 * kept so the caller refuses a negative value as the Shell's TIMEOUT does.
 */
static inline int addif_seconds(const char *text, long *out)
{
    unsigned long v = 0;
    int           neg = 0;
    int           digits = 0;

    if (*text == '-' || *text == '+')
        neg = (*text++ == '-');

    for (; *text >= '0' && *text <= '9'; text++)
    {
        unsigned long d = (unsigned long)(*text - '0');

        /* Digit-exact: 2147483647 is a LONG, 2147483648 is not. */
        if (v > 214748364UL || (v == 214748364UL && d > 7UL))
            return 0;
        v = v * 10UL + d;
        digits++;
    }

    if (digits == 0 || *text != '\0')
        return 0;

    *out = neg ? -(long)v : (long)v;
    return 1;
}

/* T:AddNetInterface.<id>: one log per launch, `id` the Process address, so
   two icons opened at once do not share a file. */
static inline void addif_log_name(char *buf, unsigned long size,
                                  unsigned long id)
{
    static const char hex[] = "0123456789abcdef";
    static const char head[] = "T:AddNetInterface.";
    unsigned long     n = 0;
    int               shift;

    while (head[n] != '\0' && n + 1 < size)
    {
        buf[n] = head[n];
        n++;
    }
    for (shift = 28; shift >= 0 && n + 1 < size; shift -= 4)
        buf[n++] = hex[(id >> shift) & 15UL];
    buf[n] = '\0';
}

#endif /* AMINETXDUO_ADDIF_WB_H */
