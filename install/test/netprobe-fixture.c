/*
 * Deterministic InstallNetProbe substitute for Installer scenario tests.
 * It exercises the installer's four device-tree outcomes without pretending
 * Amiberry exposes an Emu68 device tree.  The real helper is still what ships;
 * this binary exists only on the throw-away Workbench test drive.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#ifndef FIXTURE_GENET
#define FIXTURE_GENET 0
#endif
#ifndef FIXTURE_WIFI
#define FIXTURE_WIFI 0
#endif

int main(int argc, char **argv)
{
    if (argc != 2)
        return 20;
    if (strcmp(argv[1], "EMU68") == 0)
        return (FIXTURE_GENET || FIXTURE_WIFI) ? 0 : 5;
    if (strcmp(argv[1], "GENET") == 0)
        return FIXTURE_GENET ? 0 : 5;
    if (strcmp(argv[1], "WIFIPI") == 0)
        return FIXTURE_WIFI ? 0 : 5;
    return 20;
}
