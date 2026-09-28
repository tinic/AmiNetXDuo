/* Pure device-name defaults shared by the driver and host netstack tests. */
#include "aminetxduo/sana2.h"

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int name_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (lower_ascii(*a++) != lower_ascii(*b++))
            return 0;
    }
    return *a == *b;
}

ULONG ami_sana2_default_tcp_ack_max(const char *device)
{
    const char *base = device;
    const char *p;

    if (device == NULL)
        return 0;

    for (p = device; *p != '\0'; p++)
        if (*p == '/' || *p == ':')
            base = p + 1;

    if (name_equal(base, "anxwifipi.device") ||
        name_equal(base, "wifipi.device"))
        return 11680UL;

    return 0;
}
