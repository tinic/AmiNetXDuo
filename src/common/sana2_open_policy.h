/*
 * Only iComp's X-Surf SANA-II drivers select private AmiTCP copy callbacks
 * when the AMITCP public port is visible during OpenDevice(). Keep this
 * test independent of Amiga headers so the same policy is host-tested.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_OPEN_POLICY_H
#define AMINETXDUO_SANA2_OPEN_POLICY_H

static int ami_sana2_ascii_equal(const char *left, const char *right)
{
    unsigned char a;
    unsigned char b;

    do
    {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return 0;
    } while (a != 0);

    return 1;
}

static int ami_sana2_needs_amitcp_guard(const char *name)
{
    const char *base = name;
    const char *scan;

    if (name == 0)
        return 0;

    for (scan = name; *scan != '\0'; scan++)
    {
        if (*scan == '/' || *scan == ':')
            base = scan + 1;
    }

    for (scan = "x-surf"; *scan != '\0'; scan++, base++)
    {
        if (((unsigned char)*base | 0x20U) != (unsigned char)*scan)
            return 0;
    }

    if (*base == '-')
    {
        base++;
        if ((base[0] != '1' && base[0] != '5') ||
            base[1] != '0' || base[2] != '0')
            return 0;
        base += 3;
    }

    return ami_sana2_ascii_equal(base, ".device");
}

#endif /* AMINETXDUO_SANA2_OPEN_POLICY_H */
