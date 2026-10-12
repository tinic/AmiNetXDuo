/* A kept interface file follows the AmiNetXDuo: assign: only a DEVICE line
   naming one of our drivers in this drawer changes, and only its value. */
#include <stdio.h>
#include <string.h>

#include "devicehome.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* Stands in for SameLock: the drawers the installer names as its own. */
static int ours(const char *dir, void *ctx)
{
    const char *const *mine = (const char *const *)ctx;
    for (; *mine != NULL; mine++)
        if (strcmp(dir, *mine) == 0) return 1;
    return 0;
}

static const char *const drawer[] = {
    "SYS:AmiNetXDuo/Devs/Networks", "DH0:AmiNetXDuo/Devs/Networks", NULL
};

static void expect(const char *in, const char *want, long n, const char *what)
{
    char   out[1024];
    size_t len = 0;
    long   got = dh_rehome(in, strlen(in), out, sizeof(out), &len, ours,
                           (void *)drawer);

    check(got == n, what);
    check(len == strlen(want) && memcmp(out, want, len) == 0, what);
    if (len != strlen(want) || memcmp(out, want, len) != 0)
        printf("  got: [%.*s]\n", (int)len, out);
}

int main(void)
{
    char        dir[128];
    const char *drv;

    expect("; written by 0.30.0\n"
           "DEVICE=SYS:AmiNetXDuo/Devs/Networks/anxnet.device\n"
           "CARD=ne2000_pcmcia\n"
           "UNIT=0\r\n"
           "CONFIGURE=DHCP\n",
           "; written by 0.30.0\n"
           "DEVICE=AmiNetXDuo:Devs/Networks/anxnet.device\n"
           "CARD=ne2000_pcmcia\n"
           "UNIT=0\r\n"
           "CONFIGURE=DHCP\n",
           1, "the lab A1200's file follows the assign; nothing else moves");

    expect("DEVICE = \"DH0:AmiNetXDuo/Devs/Networks/ANXGENET.device\" ; pi\n",
           "DEVICE = \"AmiNetXDuo:Devs/Networks/anxgenet.device\" ; pi\n",
           1, "spacing, quotes and the trailing comment are kept");

    expect("device SYS:AmiNetXDuo/Devs/Networks/anxwifipi.device",
           "device AmiNetXDuo:Devs/Networks/anxwifipi.device",
           1, "no '=' and no final newline");

    expect("DEVICE=SYS:AmiNetXDuo/Devs/Networks/anxzz9000.device UNIT=0\n",
           "DEVICE=AmiNetXDuo:Devs/Networks/anxzz9000.device UNIT=0\n",
           1, "a second pair on the line is kept");

    {
        static const char same[] =
            "DEVICE=SYS:Other/Devs/Networks/anxnet.device\n"
            "DEVICE=SYS:AmiNetXDuo/Devs/Networks/a2065.device\n"
            "DEVICE=DEVS:Networks/anxnet.device\n"
            "DEVICE=anxnet.device\n"
            "DEVICE=AmiNetXDuo:Devs/Networks/anxnet.device\n"
            "DEVICE=aminetxduo:devs/networks/anxnet.device\n"
            "DEVICE=:AmiNetXDuo/Devs/Networks/anxnet.device\n"
            ";DEVICE=SYS:AmiNetXDuo/Devs/Networks/anxnet.device\n"
            "# DEVICE=SYS:AmiNetXDuo/Devs/Networks/anxnet.device\n"
            "DEVICES=SYS:AmiNetXDuo/Devs/Networks/anxnet.device\n"
            "DEVICE=\"SYS:AmiNetXDuo/Devs/Networks/anx*\"net.device\"\n"
            "DEVICE=SYS:AmiNetXDuo/Devs/Networks/anxnet.device.old\n"
            "DEVICE=\n";
        expect(same, same, 0,
               "elsewhere, third-party, relative, assign, comments, escapes: "
               "byte-identical");
    }

    check(dh_absolute_driver("Work:x/anxnet.device", 20, dir, sizeof(dir),
                             &drv) && strcmp(dir, "Work:x") == 0 &&
          strcmp(drv, "anxnet.device") == 0, "directory part");
    check(dh_absolute_driver("RAM:anxnet.device", 17, dir, sizeof(dir),
                             &drv) && strcmp(dir, "RAM:") == 0,
          "a volume root keeps its colon");
    check(!dh_absolute_driver("RAM:anxnet.device", 17, dir, 4, &drv),
          "a directory that does not fit is not offered");

    {
        static const char in[] =
            "DEVICE=SYS:AmiNetXDuo/Devs/Networks/anxnet.device\n";
        char   small[40];
        size_t len;
        check(dh_rehome(in, sizeof(in) - 1, small, sizeof(small), &len, ours,
                        (void *)drawer) == -1, "overflow is reported");
    }

    if (failures == 0) printf("devicehome: all passed\n");
    return failures != 0;
}
