/*
 * InstallNetProbe: the PC Card decision; CheckNetDevice: the slot report.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netprobe_card.h"
#include "../netdev/netdev_wireless.h"

#include <stddef.h>

#define CIS_FUNC_LAN            6
#define CIS_FUNCE_LAN_TECH      0x01
#define CIS_LAN_TECH_WIRELESS   0x07
#define CISTPL_NULL             0x00
#define CISTPL_VERS_1           0x15
#define CISTPL_MANFID           0x20
#define CISTPL_FUNCID           0x21
#define CISTPL_FUNCE            0x22
#define CISTPL_END              0xff

static int wireless(const NetprobeCis *cis)
{
    if (cis->have_funce && cis->funce[0] == CIS_FUNCE_LAN_TECH &&
        cis->funce[1] == CIS_LAN_TECH_WIRELESS)
        return 1;
    return cis->have_manf && netdev_wireless_id(cis->manf, cis->prod);
}

/* Wired Xircom identities from the packaged xircomce2.device 1.1. */
static const uint32_t xircom_ids[] =
{
    0x0089010aUL,
    0x0089110aUL,
    0x01050108UL,
    0x0105010aUL,
    0x0105010bUL,
    0x0105110aUL,
    0x0105110bUL,
    0x0105110cUL,
    0x0105110dUL,
    0x0138110aUL,
    0x0183010aUL,
    0x01bf010aUL,
    0
};

static int has_id(const uint32_t *ids, uint32_t id)
{
    while (*ids != 0)
        if (*ids++ == id)
            return 1;
    return 0;
}

int netprobe_pc_wireless(const NetprobeCis *cis)
{
    return wireless(cis);
}

int netprobe_pc_kind(const NetprobeCis *cis)
{
    if (cis->have_manf)
    {
        uint32_t id = ((uint32_t)cis->manf << 16) | cis->prod;
        if (netdev_wireless_id(cis->manf, cis->prod))
            return NETPROBE_PC_NONE;
        if (has_id(xircom_ids, id))
            return NETPROBE_PC_XIRCOM;
    }
    /* The EtherLink III rows, netdev_cards.c, by MANFID alone as the driver
       matches them. */
    if (cis->have_manf && cis->manf == 0x0101)
    {
        if (cis->prod == 0x0589)
            return NETPROBE_PC_3C589;
        if (cis->prod == 0x0556)
            return NETPROBE_PC_3CCFEM556;
        if (cis->prod == 0x0035)
            return NETPROBE_PC_3CXEM556;
    }
    /* Only a card that says LAN.  The driver also tries a card with no
       FUNCID, or a multifunction one; the probe does not call either an
       NE2000, so a modem, a disk or an SRAM card is never preselected. */
    if (!cis->have_funcid || cis->funcid != CIS_FUNC_LAN)
        return NETPROBE_PC_NONE;
    return wireless(cis) ? NETPROBE_PC_NONE : NETPROBE_PC_NE2000;
}

/* The next tuple of a primary chain from *pos: its code, body offset and
   link.  Zero at END, a 0xff link, or a body running past size.  Every call
   moves *pos forward, so a walk is bounded by size. */
static int next_tuple(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                      uint32_t size, uint32_t *pos, uint8_t *code,
                      uint32_t *body, uint8_t *link)
{
    while (*pos + 1 < size)
    {
        *code = read(ctx, *pos);
        if (*code == CISTPL_END)
            return 0;
        if (*code == CISTPL_NULL)
        {
            (*pos)++;
            continue;
        }
        *link = read(ctx, *pos + 1);
        if (*link == 0xff)
            return 0;
        *body = *pos + 2;
        if (*body + *link > size)
            return 0;
        *pos = *body + *link;
        return 1;
    }
    return 0;
}

int netprobe_cis_walk(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                      uint32_t size, NetprobeCis *cis)
{
    uint32_t pos = 0;
    uint32_t body;
    uint8_t  code;
    uint8_t  link;

    while (next_tuple(read, ctx, size, &pos, &code, &body, &link))
    {
        if (code == CISTPL_MANFID && link >= 4 && !cis->have_manf)
        {
            cis->manf = (uint16_t)(((uint16_t)read(ctx, body + 1) << 8) |
                                   read(ctx, body));
            cis->prod = (uint16_t)(((uint16_t)read(ctx, body + 3) << 8) |
                                   read(ctx, body + 2));
            cis->have_manf = 1;
        }
        else if (code == CISTPL_FUNCID && link >= 1 && !cis->have_funcid)
        {
            cis->funcid = read(ctx, body);
            cis->have_funcid = 1;
        }
        else if (code == CISTPL_FUNCE && link >= 2 && !cis->have_funce)
        {
            cis->funce[0] = read(ctx, body);
            cis->funce[1] = read(ctx, body + 1);
            cis->have_funce = 1;
        }
    }
    return cis->have_manf || cis->have_funcid;
}

/* ------------------------------------------------- CheckNetDevice's report */

/* Appends to a fixed buffer; a full one keeps its terminator. */
typedef struct
{
    char    *buf;
    uint32_t size;
    uint32_t used;
} Text;

static int put_c(Text *t, char c)
{
    if (t->used + 1 >= t->size)
        return 0;
    t->buf[t->used++] = c;
    t->buf[t->used] = '\0';
    return 1;
}

static void put_s(Text *t, const char *s)
{
    while (*s != '\0' && put_c(t, *s))
        s++;
}

static void put_hex(Text *t, uint32_t v, int digits)
{
    static const char hex[] = "0123456789abcdef";

    put_c(t, '$');
    while (digits-- > 0)
        put_c(t, hex[(v >> (digits * 4)) & 0x0f]);
}

static void put_dec(Text *t, uint32_t v)
{
    char     d[10];
    unsigned n = 0;

    do
    {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < sizeof(d));
    while (n > 0)
        put_c(t, d[--n]);
}

static uint8_t printable(uint8_t c)
{
    return ((c >= 0x20 && c < 0x7f) || c >= 0xa0) ? c : (uint8_t)'?';
}

int netprobe_vers1_text(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                        uint32_t body, uint32_t link, char *out,
                        uint32_t outsize)
{
    /* Room kept for "..." and the terminator. */
    Text     t = { out, outsize > 4 ? outsize - 4 : 0, 0 };
    uint32_t i;
    int      strings = 0;
    int      open = 0;

    if (outsize == 0)
        return 0;
    out[0] = '\0';
    /* TPLLV1_MAJOR, TPLLV1_MINOR, then NUL-terminated strings to 0xff. */
    for (i = 2; i < link; i++)
    {
        uint8_t c = read(ctx, body + i);

        if (c == 0xff)
            break;
        if (!open)
        {
            if (strings != 0)
                put_s(&t, ", ");
            if (!put_c(&t, '"'))
                break;
            open = 1;
            strings++;
        }
        if (c == 0x00)
        {
            if (!put_c(&t, '"'))
                break;
            open = 0;
            continue;
        }
        if (!put_c(&t, (char)printable(c)))
            break;
    }
    if (open || (i < link && read(ctx, body + i) != 0xff))
    {
        /* Cut short: the string has no NUL, or the text did not fit. */
        t.size = outsize;
        put_s(&t, open ? "\"..." : "...");
    }
    return strings != 0;
}

int netprobe_cis_vers1(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                       uint32_t size, char *out, uint32_t outsize)
{
    uint32_t pos = 0;
    uint32_t body;
    uint8_t  code;
    uint8_t  link;

    if (outsize != 0)
        out[0] = '\0';
    while (next_tuple(read, ctx, size, &pos, &code, &body, &link))
        if (code == CISTPL_VERS_1)
            return netprobe_vers1_text(read, ctx, body, link, out, outsize);
    return 0;
}

/* The anxnet.device rows carry the CARD= name.  Only the drivers the
   archive installs; any other kind has none. */
const char *netprobe_pc_driver(int kind)
{
    switch (kind)
    {
    case NETPROBE_PC_NE2000:    return "anxnet.device, CARD=pcmcia";
    case NETPROBE_PC_3C589:     return "anxnet.device, CARD=3c589";
    case NETPROBE_PC_3CCFEM556: return "anxnet.device, CARD=3ccfem556";
    case NETPROBE_PC_3CXEM556:  return "anxnet.device, CARD=3cxem556";
    case NETPROBE_PC_XIRCOM:    return "xircomce2.device";
    default:                    return NULL;
    }
}

/* PC Card Standard function codes, TPLFID_FUNCTION. */
static const char *const funcid_names[] =
{
    "multifunction", "memory", "serial port", "parallel port",
    "fixed disk", "video", "LAN", "AIMS", "SCSI", "security"
};

void netprobe_slot_report(const NetprobeSlot *slot,
                          void (*line)(void *ctx, const char *text),
                          void *ctx)
{
    static char        buf[16 + NETPROBE_VERS1_MAX + NETPROBE_OWNER_MAX];
    const NetprobeCis *cis = &slot->cis;
    const char        *driver;
    Text               t;

    t.buf = buf;
    t.size = sizeof(buf);

    if (slot->slot == NETPROBE_SLOT_NONE)
    {
        line(ctx, "No card.resource; no PCMCIA slot.");
        return;
    }
    if (slot->slot == NETPROBE_SLOT_EMPTY)
    {
        line(ctx, "No card.");
        return;
    }

    t.used = 0;
    buf[0] = '\0';
    if (slot->slot == NETPROBE_SLOT_OWNED)
    {
        put_s(&t, "Card present, owned by ");
        put_s(&t, slot->owner[0] != '\0' ? slot->owner : "another driver");
        put_s(&t, "; CIS read from attribute memory.");
    }
    else
        put_s(&t, "Card present, slot free; CIS read through card.resource.");
    line(ctx, buf);

    t.used = 0;
    buf[0] = '\0';
    if (cis->have_manf)
    {
        put_s(&t, "CISTPL_MANFID: manufacturer ");
        put_hex(&t, cis->manf, 4);
        put_s(&t, ", product ");
        put_hex(&t, cis->prod, 4);
        put_c(&t, '.');
    }
    else
        put_s(&t, "No CISTPL_MANFID.");
    line(ctx, buf);

    t.used = 0;
    buf[0] = '\0';
    if (slot->have_vers1)
    {
        put_s(&t, "CISTPL_VERS_1: ");
        put_s(&t, slot->vers1);
        put_c(&t, '.');
    }
    else
        put_s(&t, "No CISTPL_VERS_1.");
    line(ctx, buf);

    t.used = 0;
    buf[0] = '\0';
    if (cis->have_funcid)
    {
        put_s(&t, "CISTPL_FUNCID ");
        put_dec(&t, cis->funcid);
        if (cis->funcid < sizeof(funcid_names) / sizeof(funcid_names[0]))
        {
            put_s(&t, " (");
            put_s(&t, funcid_names[cis->funcid]);
            put_c(&t, ')');
        }
        put_c(&t, '.');
    }
    else
        put_s(&t, "No CISTPL_FUNCID.");
    line(ctx, buf);

    t.used = 0;
    buf[0] = '\0';
    if (cis->have_funce)
    {
        put_s(&t, "First CISTPL_FUNCE: type ");
        put_hex(&t, cis->funce[0], 2);
        put_s(&t, ", value ");
        put_hex(&t, cis->funce[1], 2);
        put_c(&t, '.');
    }
    else
        put_s(&t, "No CISTPL_FUNCE.");
    line(ctx, buf);

    t.used = 0;
    buf[0] = '\0';
    driver = netprobe_pc_driver(netprobe_pc_kind(cis));
    put_s(&t, "Driver: ");
    put_s(&t, driver != NULL ? driver : "none in this archive");
    put_c(&t, '.');
    line(ctx, buf);
}
