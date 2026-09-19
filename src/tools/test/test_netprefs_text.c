#include "netprefs_text.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int yes, const char *what)
{
    if (!yes) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

int main(void)
{
    static const char original[] =
        "# hand edited\n"
        "DEVICE = old.device\n"
        "FILTER = EVERYTHING\n"
        "address = 10.0.0.2\n"
        "ADDRESS = duplicate\n"
        "RXBUFFER=13312\n";
    NpTextField fields[] = {
        { "DEVICE", "new.device", 0 },
        { "ADDRESS", NULL, 0 },
        { "CONFIGURE", "DHCP", 0 }
    };
    char out[512];
    size_t len = 0;
    int commented, wildcard;

    check(np_text_patch(original, sizeof(original) - 1, fields, 3,
                        out, sizeof(out), &len), "patch fits");
    out[len] = '\0';
    check(strstr(out, "# hand edited\n") != NULL, "comment preserved");
    check(strstr(out, "FILTER = EVERYTHING\n") != NULL, "unknown key preserved");
    check(strstr(out, "RXBUFFER=13312\n") != NULL, "advanced key preserved");
    check(strstr(out, "DEVICE = new.device\n") != NULL, "device replaced");
    check(strstr(out, "ADDRESS") == NULL && strstr(out, "address") == NULL,
          "removed key and duplicate gone");
    check(strstr(out, "CONFIGURE = DHCP\n") != NULL, "missing key appended");

    {
        static const char two_old[] =
            "ADDRESS6 = 2001:db8::1/64\n"
            "ADDRESS6 = 2001:db8::2/64\n";
        NpTextField two[] = {
            { "ADDRESS6", "2001:db8::10/64", 0 },
            { "ADDRESS6", "2001:db8::20/64", 0 }
        };
        len = 0;
        check(np_text_patch(two_old, sizeof(two_old) - 1, two, 2,
                            out, sizeof(out), &len), "two-value patch fits");
        out[len] = '\0';
        check(strcmp(out,
                     "ADDRESS6 = 2001:db8::10/64\n"
                     "ADDRESS6 = 2001:db8::20/64\n") == 0,
              "two same-key values retain order");
    }

    {
        static const char line[] = "C:AddNetInterface DEVS:NetInterfaces/eth0 QUIET\n";
        check(np_startup_line(line, sizeof(line) - 1,
          "eth0", &commented, &wildcard) && !commented && !wildcard,
          "system exact startup line");
    }
    {
        static const char line[] = " ; AmiNetXDuo:C/AddNetInterface eth0 QUIET\n";
        check(np_startup_line(line, sizeof(line) - 1,
          "eth0", &commented, &wildcard) && commented && !wildcard,
          "drawer commented startup line");
    }
    {
        static const char line[] =
            "C:AddNetInterface DEVS:NetInterfaces/~(#?.info) QUIET\n";
        check(np_startup_line(line, sizeof(line) - 1,
          "eth0", &commented, &wildcard) && !commented && wildcard,
          "Roadshow wildcard startup line");
    }
    {
        static const char line[] =
            "C:AddNetInterface DEVS:NetInterfaces/wifi0 QUIET\n";
        check(!np_startup_line(line, sizeof(line) - 1,
          "eth0", &commented, &wildcard), "different interface ignored");
    }
    return failures != 0;
}
