/*
 * AmiNetXDuo -- IPv6 addresses to and from text.
 *
 * Compiled whether the tree has AMINETXDUO_IPV6 or not, and in its own file
 * rather than config_text.c.  Both halves are deliberate.
 *
 * Unconditional: the Shell commands are one binary for a library built either
 * way, so a command handed "::1" on an IPv4-only machine still has to tell a
 * valid address from a typo.  It used to ask the library's inet_pton(), which
 * answers EAFNOSUPPORT there -- `arp ::1` called a good address "not an
 * address" and `nslookup ::1` asked the DNS for a name spelt like one.
 *
 * Its own file: a pulled archive member is kept whole and the library link
 * does not collect sections, so in config_text.c these two came in with
 * ami_cfg_trim() and cost a floor build 3.4 KB (netstack_test .text 162,936
 * -> 166,316).  Here nothing in an IPv4-only library refers to them and the
 * member is never pulled.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

/*
 * RFC 4291 §2.2 text form.  Written from the grammar rather than adapted from
 * a BSD inet_pton(), because the two callers want slightly different dialects
 * (see include/aminetxduo/config.h) and this file uses no libc.
 *
 * The awkward parts of the format:
 *
 *   - "::" stands for one or more groups of zeroes and may appear at most
 *     once.  Groups are collected into a flat array as they are read, the
 *     position of "::" is remembered, and the array is shifted right at the
 *     end.  This is smaller than placing them as they arrive and makes "::" at
 *     either end fall out for free.
 *   - a trailing dotted quad ("::ffff:192.168.1.1") occupies the last two
 *     groups.  Legal only in the last position, and only with four octets.
 *   - a group is one to four hex digits.  Five is an error, not a truncation.
 */

static BOOL ip6_hex_digit(char c, ULONG *value)
{
    if (c >= '0' && c <= '9')
        *value = (ULONG)(c - '0');
    else if (c >= 'a' && c <= 'f')
        *value = (ULONG)(c - 'a') + 10UL;
    else if (c >= 'A' && c <= 'F')
        *value = (ULONG)(c - 'A') + 10UL;
    else
        return FALSE;

    return TRUE;
}

static BOOL ami_cfg_parse_ip6_inner(const char *text,
                                    ULONG out[AMI_CFG_IP6_WORDS],
                                    ULONG *prefix_out,
                                    char *zone_out, ULONG zone_len)
{
    UWORD       group[8];
    const char *s = text;
    ULONG       count     = 0;
    LONG        gap       = -1;     /* index the "::" run starts at        */
    ULONG       prefix    = 0;
    BOOL        have_pfx  = FALSE;
    ULONG       i;

    if (s == NULL)
        return FALSE;

    while (*s == ' ' || *s == '\t')
        s++;

    /* A leading "::" is the only way a leading ':' is legal. */
    if (s[0] == ':')
    {
        if (s[1] != ':')
            return FALSE;
        gap = 0;
        s += 2;
    }

    while (*s != '\0' && *s != '/' && *s != '%' && *s != ' ' && *s != '\t')
    {
        ULONG value  = 0;
        ULONG digits = 0;
        ULONG digit;

        /* A dotted quad here consumes the last two groups and ends the
           address.  Detected by scanning ahead for a '.' before the next
           ':' -- the digits themselves are ambiguous until then. */
        {
            const char *look = s;

            while (*look != '\0' && *look != ':' && *look != '/' &&
                   *look != '%' &&
                   *look != ' ' && *look != '\t' && *look != '.')
                look++;

            if (*look == '.')
            {
                ULONG v4 = 0;
                char  quad[16];
                ULONG n = 0;

                while (s[n] != '\0' && s[n] != '/' && s[n] != '%' && s[n] != ' ' &&
                       s[n] != '\t')
                {
                    if (n >= sizeof(quad) - 1)
                        return FALSE;
                    quad[n] = s[n];
                    n++;
                }
                quad[n] = '\0';

                /* config_text.c's strict dotted quad, rather than its private
                   splitter: same four octets, one fewer shared symbol. */
                if (!ami_config_parse_ip(quad, &v4))
                    return FALSE;

                if (count + 2 > 8)
                    return FALSE;

                group[count++] = (UWORD)(v4 >> 16);
                group[count++] = (UWORD)(v4 & 0xffffUL);

                s += n;
                break;
            }
        }

        while (ip6_hex_digit(*s, &digit))
        {
            if (digits >= 4)
                return FALSE;
            value = (value << 4) | digit;
            digits++;
            s++;
        }

        if (digits == 0 || count >= 8)
            return FALSE;

        group[count++] = (UWORD)value;

        if (*s != ':')
            break;

        s++;

        if (*s == ':')
        {
            if (gap >= 0)
                return FALSE;       /* a second "::" */
            gap = (LONG)count;
            s++;

            /* "1::" ends here; anything else continues with a group. */
            if (*s == '\0' || *s == '/' || *s == '%' || *s == ' ' || *s == '\t')
                break;
        }
        else if (*s == '\0' || *s == '/' || *s == '%' || *s == ' ' || *s == '\t')
        {
            return FALSE;           /* a trailing single ':' */
        }
    }

    /*
     * RFC 4007 11: "<address>%<zone_id>", before any prefix. A caller that
     * cannot carry a zone refuses one rather than dropping it -- a link-local
     * address whose zone was silently discarded names a different destination,
     * and would be sent on whichever interface the stack guessed.
     *
     * The zone is handed back as text, not resolved: this file is the
     * standalone text layer and has no library base to call if_nametoindex()
     * through. "SHOULD support at least numerical indices ... MAY support
     * other kinds of non-null strings", so the caller decides which it takes.
     */
    if (*s == '%')
    {
        ULONG n = 0;

        if (zone_out == NULL || zone_len == 0)
            return FALSE;

        s++;
        while (*s != '\0' && *s != '/' && *s != ' ' && *s != '\t')
        {
            if (n + 1 >= zone_len)
                return FALSE;       /* truncation would name another zone */
            zone_out[n++] = *s++;
        }

        if (n == 0)
            return FALSE;           /* "%" with nothing after it */

        zone_out[n] = '\0';
    }

    if (*s == '/')
    {
        ULONG digits = 0;

        if (prefix_out == NULL)
            return FALSE;           /* inet_pton() dialect: no prefix allowed */

        s++;
        while (*s >= '0' && *s <= '9')
        {
            prefix = prefix * 10UL + (ULONG)(*s - '0');
            if (prefix > 128UL)
                return FALSE;
            digits++;
            s++;
        }

        if (digits == 0)
            return FALSE;

        have_pfx = TRUE;
    }

    while (*s == ' ' || *s == '\t')
        s++;

    if (*s != '\0')
        return FALSE;

    if (gap < 0)
    {
        if (count != 8)
            return FALSE;
    }
    else
    {
        /*
         * "::" must stand for at least one group -- "1:2:3:4:5:6:7::8" has
         * eight groups already and is malformed, not merely redundant.
         */
        if (count >= 8)
            return FALSE;

        for (i = count; i > (ULONG)gap; i--)
            group[i - 1 + (8 - count)] = group[i - 1];
        for (i = (ULONG)gap; i < (ULONG)gap + (8 - count); i++)
            group[i] = 0;
    }

    if (out != NULL)
    {
        for (i = 0; i < AMI_CFG_IP6_WORDS; i++)
            out[i] = ((ULONG)group[i * 2] << 16) | (ULONG)group[i * 2 + 1];
    }

    if (prefix_out != NULL && have_pfx)
        *prefix_out = prefix;

    return TRUE;
}

BOOL ami_config_parse_ip6(const char *text, ULONG out[AMI_CFG_IP6_WORDS],
                          ULONG *prefix_out)
{
    /* No zone buffer, so a zoned address is refused rather than shortened. */
    return ami_cfg_parse_ip6_inner(text, out, prefix_out, NULL, 0);
}

BOOL ami_config_parse_ip6_zone(const char *text, ULONG out[AMI_CFG_IP6_WORDS],
                               ULONG *prefix_out, char *zone_out,
                               ULONG zone_len)
{
    if (zone_out != NULL && zone_len > 0)
        zone_out[0] = '\0';

    return ami_cfg_parse_ip6_inner(text, out, prefix_out, zone_out, zone_len);
}

VOID ami_config_format_ip6_zone(const ULONG addr[AMI_CFG_IP6_WORDS],
                                const char *zone, char *buf, ULONG buflen)
{
    ULONG used;
    ULONG i;

    if (buf == NULL || buflen == 0)
        return;

    if (zone == NULL || zone[0] == '\0')
    {
        ami_config_format_ip6(addr, buf, buflen);
        return;
    }

    if (buflen < AMI_CFG_IP6_ZONE_STRLEN)
    {
        buf[0] = '\0';
        return;
    }

    ami_config_format_ip6(addr, buf, buflen);
    if (buf[0] == '\0')
        return;

    for (used = 0; buf[used] != '\0'; used++)
        ;

    buf[used++] = '%';
    for (i = 0; zone[i] != '\0' && used + 1 < buflen; i++)
        buf[used++] = zone[i];
    buf[used] = '\0';
}

VOID ami_config_format_ip6(const ULONG addr[AMI_CFG_IP6_WORDS],
                           char *buf, ULONG buflen)
{
    char  tmp[AMI_CFG_IP6_STRLEN];
    UWORD group[8];
    ULONG pos       = 0;
    LONG  best_at   = -1;
    ULONG best_len  = 0;
    LONG  run_at    = -1;
    ULONG run_len   = 0;
    ULONG i;

    if (buf == NULL || buflen == 0)
        return;

    buf[0] = '\0';

    if (addr == NULL || buflen < AMI_CFG_IP6_STRLEN)
        return;

    for (i = 0; i < AMI_CFG_IP6_WORDS; i++)
    {
        group[i * 2]     = (UWORD)(addr[i] >> 16);
        group[i * 2 + 1] = (UWORD)(addr[i] & 0xFFFFUL);
    }

    /* Longest run of two or more zero groups; leftmost wins a tie
       (RFC 5952 4.2.3). */
    for (i = 0; i < 8; i++)
    {
        if (group[i] == 0)
        {
            if (run_at < 0)
            {
                run_at  = (LONG)i;
                run_len = 0;
            }
            run_len++;

            if (run_len > best_len)
            {
                best_len = run_len;
                best_at  = run_at;
            }
        }
        else
        {
            run_at  = -1;
            run_len = 0;
        }
    }

    if (best_len < 2)
        best_at = -1;

    /*
     * The colon bookkeeping follows the BSD/glibc inet_ntop6 shape: one colon
     * is emitted where the elided run starts, the separator in front of the
     * group that follows the run supplies the second, and a run that reaches
     * the end of the address gets its second colon added afterwards. That
     * handles "::", "::1", "1::" and "fe80::1" with no special cases.
     */
    for (i = 0; i < 8; i++)
    {
        if (best_at >= 0 && (LONG)i >= best_at &&
            (LONG)i < best_at + (LONG)best_len)
        {
            if ((LONG)i == best_at)
                tmp[pos++] = ':';
            continue;
        }

        if (i != 0)
            tmp[pos++] = ':';

        /*
         * v4-mapped (::ffff:a.b.c.d) and the deprecated v4-compatible form are
         * written with a dotted tail (RFC 5952 5). The test is the one every
         * other stack uses, so the output matches theirs byte for byte.
         */
        if (i == 6 && best_at == 0 &&
            (best_len == 6 || (best_len == 5 && group[5] == 0xFFFF)))
        {
            char  quad[16];
            ULONG addr4 = ((ULONG)group[6] << 16) | (ULONG)group[7];
            ULONG n;

            ami_config_format_ip(addr4, quad, sizeof(quad));
            for (n = 0; quad[n] != '\0'; n++)
                tmp[pos++] = quad[n];
            break;
        }

        {
            UWORD v = group[i];
            char  digits[4];
            int   n = 0;

            do
            {
                ULONG nibble = (ULONG)(v & 0xF);

                digits[n++] = (char)((nibble < 10) ? ('0' + (int)nibble)
                                                   : ('a' + (int)nibble - 10));
                v = (UWORD)(v >> 4);
            }
            while (v != 0);

            while (n > 0)
                tmp[pos++] = digits[--n];
        }
    }

    /* A run that ran to the end of the address still owes its second colon. */
    if (best_at >= 0 && (ULONG)best_at + best_len == 8)
        tmp[pos++] = ':';

    tmp[pos] = '\0';

    ami_cfg_copy_string(buf, buflen, tmp);
}
