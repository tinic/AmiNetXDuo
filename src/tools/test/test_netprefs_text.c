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
    {
        static const char line[] =
            "Run >NIL: <NIL: C:AddNetInterface DEVS:NetInterfaces/eth0 QUIET\n";
        check(np_startup_line(line, sizeof(line) - 1,
          "eth0", &commented, &wildcard) && !commented && !wildcard,
          "Run with redirections before the command");
    }
    {
        static const char line[] =
            "C:AddNetInterface >>SYS:Unpacked/boot-genet.log DEVS:NetInterfaces/genet\n";
        check(np_startup_line(line, sizeof(line) - 1,
          "genet", &commented, &wildcard) && !commented && !wildcard,
          "redirection between the command and the name");
    }
    {
        static const char line[] =
            "C:AddNetInterface >>SYS:boot.log DEVS:NetInterfaces/genet\n";
        check(!np_startup_line(line, sizeof(line) - 1,
          "boot.log", &commented, &wildcard),
          "a redirection target is not the name");
    }
    {
        static const char aliased[] =
            "IPADDRESS=192.168.1.5\n"
            "SUBNETMASK=255.255.255.0\n"
            "PRI=3\n";
        NpTextField af[] = {
            { "ADDRESS", NULL, 0 },
            { "NETMASK", NULL, 0 },
            { "PRIORITY", "7", 0 }
        };
        len = 0;
        check(np_text_patch(aliased, sizeof(aliased) - 1, af, 3,
                            out, sizeof(out), &len), "alias patch fits");
        out[len] = '\0';
        check(strcmp(out, "PRIORITY = 7\n") == 0,
              "aliases are the GUI's keys: removed and rewritten in place");
    }
    {
        static const char iptype[] =
            "IPTYPE=2048\n"
            "CONFIGURE=DHCP\n";
        NpTextField mode[] = {
            { "CONFIGURE", "AUTO", 0 }
        };
        len = 0;
        check(np_text_patch(iptype, sizeof(iptype) - 1, mode, 1,
                            out, sizeof(out), &len), "numeric IPTYPE patch fits");
        out[len] = '\0';
        check(strcmp(out, "IPTYPE=2048\nCONFIGURE = AUTO\n") == 0,
              "numeric IPTYPE remains the SANA-II packet type");
    }
    {
        static const char iptype[] = "IPTYPE=DHCP\n";
        NpTextField mode[] = {
            { "CONFIGURE", "STATIC", 0 }
        };
        len = 0;
        check(np_text_patch(iptype, sizeof(iptype) - 1, mode, 1,
                            out, sizeof(out), &len), "alphabetic IPTYPE patch fits");
        out[len] = '\0';
        check(strcmp(out, "CONFIGURE = STATIC\n") == 0,
              "alphabetic IPTYPE is the address-mode alias");
    }
    {
        static const char iptype[] = "IPTYPE=\"0x800\"\n";
        NpTextField mode[] = {
            { "CONFIGURE", "DHCP", 0 }
        };
        len = 0;
        check(np_text_patch(iptype, sizeof(iptype) - 1, mode, 1,
                            out, sizeof(out), &len), "quoted IPTYPE patch fits");
        out[len] = '\0';
        check(strcmp(out, "IPTYPE=\"0x800\"\nCONFIGURE = DHCP\n") == 0,
              "quoted numeric IPTYPE remains the packet type");
    }

    check(np_interface_name_safe("genet0", 16), "simple interface name");
    check(np_interface_name_safe("x-surf_100.0", 16),
          "safe filename punctuation");
    check(!np_interface_name_safe("", 16), "empty name rejected");
    check(!np_interface_name_safe(".eth0", 16), "dot cannot start a name");
    check(!np_interface_name_safe("-eth0", 16), "hyphen cannot start a name");
    check(np_interface_name_safe("123456789012345", 16),
          "name which fits is accepted");
    check(!np_interface_name_safe("1234567890123456", 16),
          "limit includes the terminator");
    check(!np_interface_name_safe("eth*", 16), "wildcard rejected");
    check(!np_interface_name_safe("eth?", 16), "pattern rejected");
    check(!np_interface_name_safe("eth$foo", 16), "substitution rejected");
    check(!np_interface_name_safe("eth`foo", 16), "backtick rejected");
    check(!np_interface_name_safe("eth\"foo", 16), "quote rejected");
    check(!np_interface_name_safe("eth>ram:x", 16), "redirection rejected");
    check(!np_interface_name_safe("eth|foo", 16), "pipe rejected");
    return failures != 0;
}
