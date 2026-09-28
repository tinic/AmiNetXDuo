/* Pure device-name policy shared by the driver and the host netstack tests. */
#include "aminetxduo/sana2.h"

/* Two strings the same, ignoring case: a device name is a file name. */
BOOL ami_str_iequal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char x = *a++;
        char y = *b++;

        if (x >= 'A' && x <= 'Z')
            x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + ('a' - 'A'));
        if (x != y)
            return FALSE;
    }
    return (BOOL)(*a == *b);
}

/* The file name without its path: DEVS:Networks/x.device and x.device are
   one driver. */
const char *ami_sana2_basename(const char *device)
{
    const char *base = device;
    const char *p;

    for (p = device; *p != '\0'; p++)
    {
        if (*p == '/' || *p == ':')
            base = p + 1;
    }

    return base;
}

static BOOL ami_sana2_is_wifipi(const char *device)
{
    const char *base;

    if (device == NULL)
        return FALSE;

    base = ami_sana2_basename(device);
    return (BOOL)(ami_str_iequal(base, "anxwifipi.device") ||
                  ami_str_iequal(base, "wifipi.device"));
}

/* WiFiPi wants 128 posted reads (#107); the pool budget still caps it. */
UWORD ami_sana2_default_ip_reads(const char *device)
{
    return ami_sana2_is_wifipi(device) ? 128 : 0;
}

/* WiFiPi acknowledges once per GRO run (#89, #109); 0 = the NetX default. */
ULONG ami_sana2_default_tcp_ack_max(const char *device)
{
    return ami_sana2_is_wifipi(device) ? 11680UL : 0UL;
}
