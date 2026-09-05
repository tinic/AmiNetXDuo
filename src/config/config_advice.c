/* The words for the advice codes, linked by the C: commands only.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/config_advice.h"

#include <exec/types.h>

static const char *const ami_cfg_advice_text[] =
{
    (const char *)0,
    "Run NetSetup: it asks which card this machine has, then writes"
    " the drawer and the file.",
    "This is memory, not a limit on how many interfaces may be desc"
    "ribed.  Close a program and try again.",
    "The problems listed above it say why.  NetSetup can rewrite th"
    "e file from scratch.",
    "One file per network card goes in there.  The name of the file"
    " is the name of the card, and eth0 is the usual choice.  NetSe"
    "tup writes one.",
    "Roadshow acts on it.  This stack does not.  The line is harmle"
    "ss and can stay.",
    "Rename the file in DEVS:NetInterfaces to something 15 characte"
    "rs or shorter and use that name from now on.",
    "DEVICE names the driver for the network card, for example DEVI"
    "CE=a2065.device.  The driver itself belongs in DEVS:Networks/.",
    "Add a line such as  DEVICE = a2065.device  that names the driv"
    "er for the card, or let NetSetup write the file.",
    "Add  CONFIGURE = DHCP  to have an address handed out, or  ADDR"
    "ESS = 192.168.1.10  and  NETMASK = 255.255.255.0  to set one b"
    "y hand, or  CONFIGURE6 = AUTO  for an IPv6-only interface.",
    "This file holds NAMESERVER, DOMAIN and SEARCH lines.  The line"
    " was ignored.",
    "A routes file holds DEFAULT=<router address> for the default r"
    "oute, and DST=/VIA= pairs for anything else.  The line was ign"
    "ored.",
    "This file switches the TCP: device on or off and understands n"
    "othing else.  Write TCPHANDLER=OFF, or OFF on its own.",
    "Keep it under sixty characters.  The line was ignored.",
    "A TXT record holds at most 255 characters.  The line was ignor"
    "ed.",
    "At most eight are announced.  The ones after that were ignored"
    ".",
    "UNIT is a plain number and is 0 on almost every card.  Unit 0 "
    "was assumed.",
    "An address is four numbers from 0 to 255 with dots between the"
    "m, for example 192.168.1.10.  Write ADDRESS=DHCP to have the a"
    "ddress handed out automatically.",
    "A netmask looks like an address.  On a home network it is almo"
    "st always 255.255.255.0.",
    "The gateway is the address of the router. An address is four n"
    "umbers from 0 to 255 with dots between them, for example 192.1"
    "68.1.10.",
    "MTU is a plain number of bytes, normally 1500.  Leave it out a"
    "nd the driver decides.",
    "CONFIGURE is DHCP (let the network hand out an address), STATI"
    "C (use the ADDRESS below), AUTO (pick one without a server) or"
    " NONE (no IPv4 on this interface).  STATIC was assumed.",
    "IPTYPE is either a packet type number (2048 for Ethernet) or o"
    "ne of DHCP, STATIC and AUTO.",
    "MDNS is YES or NO.  NO was assumed.",
    "DOWNGOESOFFLINE is YES or NO.  NO was assumed.",
    "REQUIRESINITDELAY is YES or NO.  NO was assumed.",
    "HARDWAREADDRESS is six hexadecimal bytes, as in 02:00:00:12:34"
    ":56.  The card's own address was kept.",
    "STATE is UP or DOWN.  UP was assumed.",
    "An interface carries at most two static IPv6 addresses, becaus"
    "e the third slot the stack has per interface holds the link-lo"
    "cal address.  This line was ignored.",
    "CONFIGURE6 is AUTO (follow the router), DHCP (ask a DHCPv6 ser"
    "ver), STATIC (use the ADDRESS6 below), LINKLOCAL (fe80:: only)"
    " or OFF.  AUTO was assumed.",
    "A name server is given by address, not by name.  On a home net"
    "work it is usually the router, for example 192.168.1.1.",
    "This is the address of the router, and it must be on the same "
    "network as this machine. An address is four numbers from 0 to "
    "255 with dots between them, for example 192.168.1.10.",
    "Write ON or OFF.  The TCP: device was left switched on.",
    "A line is <type> <port>, for example:  _ftp._tcp  21.  The typ"
    "e is an RFC 6763 name: an underscore, up to fifteen letters, d"
    "igits or hyphens, then ._tcp or ._udp.  The line was ignored.",
    "A port is a number from 1 to 65535, and it is the port the ser"
    "ver listens on.  The line was ignored.",
    "A service name is one label, so it cannot contain a dot.  The "
    "line was ignored.",
    "The keywords an interface file understands are DEVICE, UNIT, C"
    "ONFIGURE, ADDRESS, NETMASK, GATEWAY, MTU, and CONFIGURE6, ADDR"
    "ESS6 and GATEWAY6 for IPv6.  The line was ignored.",
};

const char *ami_cfg_advice(UWORD code)
{
    if ((code == 0U) ||
        (code >= (UWORD)(sizeof ami_cfg_advice_text /
                         sizeof ami_cfg_advice_text[0])))
    {
        return (const char *)0;
    }

    return ami_cfg_advice_text[code];
}
