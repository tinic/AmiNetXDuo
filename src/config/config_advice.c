/* The words for the advice codes, linked by the C: commands only.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/config_advice.h"
/* The codes are here; ami_cfg_advice() itself is declared in config.h, so the
   compiler can check this file against what callers are told to expect. */
#include "aminetxduo/config.h"

#include <exec/types.h>

#define AMI_CFG_STRINGIFY_INNER(x) #x
#define AMI_CFG_STRINGIFY(x)       AMI_CFG_STRINGIFY_INNER(x)

static const char *const ami_cfg_advice_text[] =
{
    (const char *)0,
    (const char *)0,
    (const char *)0,
    (const char *)0,
    (const char *)0,
    (const char *)0,
    "names are at most 15 characters",
    "DEVICE names the driver, e.g. DEVICE=a2065.device",
    "e.g. DEVICE=a2065.device",
    (const char *)0,
    "keywords: NAMESERVER, DOMAIN, SEARCH; line ignored",
    "keywords: DEFAULT=<router>, DST= with VIA=; line ignored",
    "accepts TCPHANDLER=ON|OFF or ON|OFF alone",
    "under 60 characters; line ignored",
    "TXT limit 255 characters; line ignored",
    "at most 8 announced; rest ignored",
    "UNIT is a number; 0 assumed",
    "expected a.b.c.d or DHCP",
    "expected a.b.c.d, e.g. 255.255.255.0",
    "expected a.b.c.d",
    "MTU is bytes, normally 1500; default from driver",
    "CONFIGURE is DHCP, STATIC, AUTO or NONE; STATIC assumed",
    "IPTYPE is DHCP, STATIC, AUTO or NONE; numeric types ignored",
    "MDNS is YES or NO; NO assumed",
    "DOWNGOESOFFLINE is YES or NO; NO assumed",
    "REQUIRESINITDELAY is YES or NO; NO assumed",
    "HARDWAREADDRESS is 6 hex bytes, e.g. 02:00:00:12:34:56; card a"
    "ddress kept",
    "STATE is UP or DOWN; UP assumed",
    "at most 2 static IPv6 addresses; line ignored",
    "CONFIGURE6 is AUTO, DHCP, STATIC, LINKLOCAL or OFF; AUTO assumed",
    "NAMESERVER takes an address, not a name",
    "expected a.b.c.d on a local network",
    "ON or OFF; TCP: left on",
    "expected _<name>._tcp|._udp <port>, e.g. _ftp._tcp 21; line ig"
    "nored",
    "port is 1 to 65535; line ignored",
    "no dots; line ignored",
    "line ignored",
    "no DEVS:NetInterfaces drawer",
    "DEVS:NetInterfaces: no usable interface file",
    "DEVICE has no value",
    "no DEVICE line",
    "no address: no ADDRESS, CONFIGURE=DHCP or CONFIGURE6",
    "the service name is too long",
    "the txt= field is too long",
    "too many services",
    "name server taken from the interface file",
    "DEVS:Internet/name_resolution takes precedence",
    "IPREQUESTS and ARPREQUESTS are 1 to 128; default from link spe"
    "ed and memory",
    "WRITEREQUESTS is 1 to " AMI_CFG_STRINGIFY(AMI_CFG_WRITEREQUESTS_MAX)
    ", default " AMI_CFG_STRINGIFY(AMI_CFG_WRITEREQUESTS_MAX),
    "RXBUFFER is the card's receive buffer in bytes (13312 on an X-"
    "Surf); default from driver",
    "PRIORITY is -128 to 127, default 0; higher wins",
    "FILTER is LOCAL, IPANDARP or EVERYTHING (promiscuous)",
    "TCPACKMAX is bytes before an ACK, 1 to 65535; default from device",
    "TCPGROWRTT is milliseconds, 1 to 65535, default 10",
    "out of memory reading DEVS:NetInterfaces",
    "numeric IPTYPE ignored",
    "TCPWANWINDOW is bytes, 1 to 1048576; default from RXBUFFER",
    "GROFRAMES is 1 to 16, default 16",
    "ACKPACE is kbit/s, 1 to 1000000; default unpaced",
    "LINKSPEED is AUTO or 10 (Mbit/s); requires a supporting driver",
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
