/*
 * CreateAmiNetXDuoStatusReport, the half that decides what a line says.
 * See statusreport_text.h; the allowlists are the tables below.
 *
 * SPDX-License-Identifier: MIT
 */

#include "statusreport_text.h"

#include "aminetxduo/anxdiag.h"
#include "config_internal.h"

/* ---------------------------------------------------------- characters --- */

static BOOL sr_is_digit(char c)
{
    return (BOOL)(c >= '0' && c <= '9');
}

static BOOL sr_is_alpha(char c)
{
    return (BOOL)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static char sr_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static BOOL sr_in(char c, const char *set)
{
    for (; *set != '\0'; set++)
    {
        if (*set == c)
            return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- lines --- */

static char sr_line[SR_LINE_MAX];

static VOID sr_emit(SrOut *o, const char *key, const char *value)
{
    ULONG n = 0;
    ULONG v = 0;

    if (o == NULL || o->write == NULL || key == NULL)
        return;

    if (value == NULL)
        value = SR_UNAVAILABLE;

    while (key[n] != '\0' && n < SR_KEY_MAX)
    {
        sr_line[n] = key[n];
        n++;
    }
    sr_line[n++] = '=';

    /* Printable ASCII and Latin-1 pass; a control character would be a new
       line or a terminal command in somebody's bug tracker. */
    while (value[v] != '\0' && v < SR_VALUE_MAX)
    {
        unsigned char c = (unsigned char)value[v++];

        sr_line[n++] = (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0))
                       ? '?' : (char)c;
    }

    sr_line[n++] = '\n';
    sr_line[n]   = '\0';

    o->write(o->user, sr_line);
    o->lines++;
}

VOID sr_str(SrOut *o, const char *key, const char *value)
{
    sr_emit(o, key, value);
}

static VOID sr_format_ulong(char *buf, ULONG value)
{
    char  tmp[12];
    ULONG n = 0;
    ULONG i = 0;

    do
    {
        tmp[n++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    } while (value != 0UL);

    while (n > 0)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
}

VOID sr_ulong(SrOut *o, const char *key, ULONG value)
{
    char buf[12];

    sr_format_ulong(buf, value);
    sr_emit(o, key, buf);
}

static VOID sr_format_long(char *buf, LONG value)
{
    if (value < 0)
    {
        buf[0] = '-';
        sr_format_ulong(buf + 1, (ULONG)(-(value + 1)) + 1UL);
    }
    else
    {
        sr_format_ulong(buf, (ULONG)value);
    }
}

static VOID sr_format_hex(char *buf, ULONG value)
{
    static const char digits[] = "0123456789abcdef";
    int               i;

    buf[0] = '$';
    for (i = 0; i < 8; i++)
        buf[1 + i] = digits[(value >> (28 - 4 * i)) & 0xfUL];
    buf[9] = '\0';
}

VOID sr_long(SrOut *o, const char *key, LONG value)
{
    char buf[13];

    sr_format_long(buf, value);
    sr_emit(o, key, buf);
}

VOID sr_hex(SrOut *o, const char *key, ULONG value)
{
    char buf[10];

    sr_format_hex(buf, value);
    sr_emit(o, key, buf);
}

VOID sr_cat(char *dst, ULONG dstlen, const char *text)
{
    ULONG n = 0;

    if (dst == NULL || dstlen == 0UL)
        return;

    while (n < dstlen && dst[n] != '\0')
        n++;
    for (; text != NULL && *text != '\0' && n + 1UL < dstlen; text++)
        dst[n++] = *text;
    if (n < dstlen)
        dst[n] = '\0';
}

VOID sr_cat_ulong(char *dst, ULONG dstlen, ULONG value)
{
    char buf[12];

    sr_format_ulong(buf, value);
    sr_cat(dst, dstlen, buf);
}

VOID sr_cat_long(char *dst, ULONG dstlen, LONG value)
{
    char buf[13];

    sr_format_long(buf, value);
    sr_cat(dst, dstlen, buf);
}

VOID sr_cat_hex(char *dst, ULONG dstlen, ULONG value)
{
    char buf[10];

    sr_format_hex(buf, value);
    sr_cat(dst, dstlen, buf);
}

VOID sr_yesno(SrOut *o, const char *key, BOOL value)
{
    sr_emit(o, key, value ? "yes" : "no");
}

VOID sr_tee_write(APTR user, const char *line)
{
    SrTee *t = (SrTee *)user;

    (VOID)t->put(t->console, line);
    if (t->file != NULL && !t->file_failed && t->put(t->file, line) != 0)
        t->file_failed = TRUE;
}

VOID sr_version(SrOut *o, const char *key, ULONG version, ULONG revision)
{
    char  buf[24];
    ULONG n;

    sr_format_ulong(buf, version);
    for (n = 0; buf[n] != '\0'; n++)
        ;
    buf[n++] = '.';
    sr_format_ulong(buf + n, revision);

    sr_emit(o, key, buf);
}

VOID sr_addr_str(SrOut *o, const char *key, const char *value)
{
    if (o != NULL && o->addresses)
        sr_emit(o, key, value);
}

VOID sr_addr_ipv4(SrOut *o, const char *key, ULONG addr)
{
    char  buf[16];
    ULONG n = 0;
    int   i;

    if (o == NULL || !o->addresses)
        return;

    for (i = 3; i >= 0; i--)
    {
        sr_format_ulong(buf + n, (addr >> (8 * i)) & 0xffUL);
        while (buf[n] != '\0')
            n++;
        if (i > 0)
            buf[n++] = '.';
    }
    buf[n] = '\0';

    sr_emit(o, key, buf);
}

VOID sr_addr_mac(SrOut *o, const char *key, const UBYTE *mac)
{
    static const char digits[] = "0123456789abcdef";
    char              buf[18];
    int               i;

    if (o == NULL || !o->addresses || mac == NULL)
        return;

    for (i = 0; i < 6; i++)
    {
        buf[3 * i]     = digits[(mac[i] >> 4) & 0xf];
        buf[3 * i + 1] = digits[mac[i] & 0xf];
        buf[3 * i + 2] = (i < 5) ? ':' : '\0';
    }

    sr_emit(o, key, buf);
}

/* ----------------------------------------------------------------- keys --- */

VOID sr_key_part(char *dst, ULONG dstlen, const char *text)
{
    ULONG n = 0;

    if (dst == NULL || dstlen == 0UL)
        return;

    if (text != NULL)
    {
        for (; *text != '\0' && n + 1UL < dstlen; text++)
        {
            char c = *text;

            dst[n++] = (sr_is_alpha(c) || sr_is_digit(c) || c == '_' ||
                        c == '-' || c == '.') ? c : '_';
        }
    }

    if (n == 0UL && dstlen > 1UL)
        dst[n++] = '_';

    dst[n] = '\0';
}

static ULONG sr_append(char *dst, ULONG n, ULONG dstlen, const char *text)
{
    for (; text != NULL && *text != '\0' && n + 1UL < dstlen; text++)
        dst[n++] = *text;
    dst[n] = '\0';
    return n;
}

VOID sr_key(char *dst, ULONG dstlen, const char *a, const char *b,
            const char *c)
{
    ULONG n = 0;

    if (dst == NULL || dstlen == 0UL)
        return;

    dst[0] = '\0';
    n = sr_append(dst, n, dstlen, a);
    if (b != NULL)
    {
        n = sr_append(dst, n, dstlen, ".");
        n = sr_append(dst, n, dstlen, b);
        if (c != NULL)
        {
            n = sr_append(dst, n, dstlen, ".");
            (VOID)sr_append(dst, n, dstlen, c);
        }
    }
}

VOID sr_key_index(char *dst, ULONG dstlen, const char *prefix, ULONG index,
                  const char *suffix)
{
    char  num[12];
    ULONG n = 0;

    if (dst == NULL || dstlen == 0UL)
        return;

    sr_format_ulong(num, index);
    dst[0] = '\0';
    n = sr_append(dst, n, dstlen, prefix);
    n = sr_append(dst, n, dstlen, ".");
    n = sr_append(dst, n, dstlen, num);
    if (suffix != NULL)
    {
        n = sr_append(dst, n, dstlen, ".");
        (VOID)sr_append(dst, n, dstlen, suffix);
    }
}

/* ------------------------------------------------------------- decoding --- */

/* ExecBase->AttnFlags bits, from <exec/execbase.h>.  Restated so the host
   test needs no NDK; AFB_68060 is not in the 3.1 NDK at all. */
#define SR_AFB_68010    0
#define SR_AFB_68020    1
#define SR_AFB_68030    2
#define SR_AFB_68040    3
#define SR_AFB_68881    4
#define SR_AFB_68882    5
#define SR_AFB_FPU40    6
#define SR_AFB_68060    7

#define SR_BIT(b)       (1UL << (b))

const char *sr_cpu_name(ULONG attn)
{
    if (attn & SR_BIT(SR_AFB_68060)) return "68060";
    if (attn & SR_BIT(SR_AFB_68040)) return "68040";
    if (attn & SR_BIT(SR_AFB_68030)) return "68030";
    if (attn & SR_BIT(SR_AFB_68020)) return "68020";
    if (attn & SR_BIT(SR_AFB_68010)) return "68010";
    return "68000";
}

const char *sr_fpu_name(ULONG attn)
{
    /* The 68040's and 68060's own FPU sets the 68881/68882 bits too. */
    if (attn & SR_BIT(SR_AFB_FPU40))
        return (attn & SR_BIT(SR_AFB_68060)) ? "68060" : "68040";
    if (attn & SR_BIT(SR_AFB_68882)) return "68882";
    if (attn & SR_BIT(SR_AFB_68881)) return "68881";
    return "none";
}

/* ------------------------------------------------------ interface files --- */

/* What a value has to look like to be printed.  One that does not is printed
   as SR_INVALID, never as itself. */
enum
{
    SR_V_ENUM = 0,      /* one word from the row's list                     */
    SR_V_NUMBER,        /* a decimal or $hex number                          */
    SR_V_DRIVER,        /* a device file name, perhaps with a path           */
    SR_V_WORD,          /* letters, digits, '-', '_'                         */
    SR_V_ADDRESS,       /* v4, v6 or MAC notation, or a word like DHCP       */
    SR_V_HOSTNAME       /* letters, digits, '-', '_', '.'                    */
};

/* Which class of key it is. */
#define SR_ALWAYS       0
#define SR_ADDRESSES    1

static const char *const sr_enum_iptype[] =
{
    "dhcp", "bootp", "auto", "fastauto", "zeroconf", "linklocal", "static",
    "manual", "none", "off", "no", "disabled", NULL
};

static const char *const sr_enum_ip6type[] =
{
    "off", "no", "none", "disabled", "linklocal", "link-local", "local",
    "auto", "slaac", "stateless", "ra", "static", "manual", "dhcp", "dhcpv6",
    NULL
};

static const char *const sr_enum_bool[] =
{
    "yes", "true", "on", "1", "no", "false", "off", "0", NULL
};

static const char *const sr_enum_state[] =
{
    "up", "online", "down", "offline", NULL
};

static const char *const sr_enum_filter[] =
{
    "everything", "local", "ipandarp", NULL
};

typedef struct SrIfKey
{
    const char        *keyword;     /* as the parser spells it (any case)  */
    const char        *key;         /* what the report calls it            */
    UBYTE              cls;         /* SR_ALWAYS or SR_ADDRESSES           */
    UBYTE              shape;       /* SR_V_*                              */
    const char *const *words;       /* SR_V_ENUM only                      */
} SrIfKey;

/*
 * THE ALLOWLIST.  A keyword not in this table is never printed, whatever its
 * value; the report says only how many there were.  Every keyword
 * src/config/config_parse.c knows is either here or deliberately left out:
 * the ones left out (ARPTYPE, DEBUG, ALIAS, LINKSTATUSCOMMAND and the rest of
 * Roadshow's inert set) are ignored by the stack, and anything else in the
 * file -- a PASSWORD, a KEY, whatever a third-party tool left there -- is not
 * the stack's business or the report's.
 */
static const SrIfKey sr_if_keys[] =
{
    { "device",            "device",            SR_ALWAYS,    SR_V_DRIVER,   NULL },
    { "card",              "card",              SR_ALWAYS,    SR_V_WORD,     NULL },
    { "unit",              "unit",              SR_ALWAYS,    SR_V_NUMBER,   NULL },
    { "configure",         "configure",         SR_ALWAYS,    SR_V_ENUM,     sr_enum_iptype },
    { "iptype",            "configure",         SR_ALWAYS,    SR_V_ENUM,     sr_enum_iptype },
    { "configure6",        "configure6",        SR_ALWAYS,    SR_V_ENUM,     sr_enum_ip6type },
    { "iptype6",           "configure6",        SR_ALWAYS,    SR_V_ENUM,     sr_enum_ip6type },
    { "state",             "state",             SR_ALWAYS,    SR_V_ENUM,     sr_enum_state },
    { "mtu",               "mtu",               SR_ALWAYS,    SR_V_NUMBER,   NULL },
    { "mdns",              "mdns",              SR_ALWAYS,    SR_V_ENUM,     sr_enum_bool },
    { "priority",          "priority",          SR_ALWAYS,    SR_V_NUMBER,   NULL },
    { "pri",               "priority",          SR_ALWAYS,    SR_V_NUMBER,   NULL },
    { "filter",            "filter",            SR_ALWAYS,    SR_V_ENUM,     sr_enum_filter },
    { "downgoesoffline",   "downgoesoffline",   SR_ALWAYS,    SR_V_ENUM,     sr_enum_bool },
    { "requiresinitdelay", "requiresinitdelay", SR_ALWAYS,    SR_V_ENUM,     sr_enum_bool },
    { "iprequests",        "iprequests",        SR_ALWAYS,    SR_V_NUMBER,   NULL },
    { "arprequests",       "arprequests",       SR_ALWAYS,    SR_V_NUMBER,   NULL },
    { "writerequests",     "writerequests",     SR_ALWAYS,    SR_V_NUMBER,   NULL },
    { "rxbuffer",          "rxbuffer",          SR_ALWAYS,    SR_V_NUMBER,   NULL },

    { "address",           "address",           SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "ipaddress",         "address",           SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "netmask",           "netmask",           SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "subnetmask",        "netmask",           SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "gateway",           "gateway",           SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "address6",          "address6",          SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "ipaddress6",        "address6",          SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "gateway6",          "gateway6",          SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "hardwareaddress",   "hardwareaddress",   SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "nameserver",        "nameserver",        SR_ADDRESSES, SR_V_ADDRESS,  NULL },
    { "id",                "id",                SR_ADDRESSES, SR_V_HOSTNAME, NULL },
    { "domain",            "domain",            SR_ADDRESSES, SR_V_HOSTNAME, NULL },

    { NULL,                NULL,                0,            0,             NULL }
};

static const SrIfKey *sr_if_key(const char *keyword)
{
    const SrIfKey *k;

    for (k = sr_if_keys; k->keyword != NULL; k++)
    {
        if (ami_cfg_stricmp(keyword, k->keyword) == 0)
            return k;
    }
    return NULL;
}

/* Every character of `value` in the classes given, and at most `max` long. */
static BOOL sr_made_of(const char *value, ULONG max, BOOL alpha, BOOL digit,
                       const char *extra)
{
    ULONG n = 0;

    if (value == NULL || *value == '\0')
        return FALSE;

    for (; *value != '\0'; value++, n++)
    {
        char c = *value;

        if (n >= max)
            return FALSE;
        if (alpha && sr_is_alpha(c))
            continue;
        if (digit && sr_is_digit(c))
            continue;
        if (extra != NULL && sr_in(c, extra))
            continue;
        return FALSE;
    }
    return TRUE;
}

static char sr_enum_buf[16];

/* The value to print for `value` under row `k`, or SR_INVALID. */
static const char *sr_shape(const SrIfKey *k, const char *value)
{
    switch (k->shape)
    {
    case SR_V_ENUM:
    {
        const char *const *w;

        for (w = k->words; w != NULL && *w != NULL; w++)
        {
            if (ami_cfg_stricmp(value, *w) == 0)
            {
                /* The table's own spelling, so nothing of the file's goes
                   through but the choice. */
                ULONG i;

                for (i = 0; (*w)[i] != '\0' && i + 1 < sizeof(sr_enum_buf); i++)
                    sr_enum_buf[i] = sr_lower((*w)[i]);
                sr_enum_buf[i] = '\0';
                return sr_enum_buf;
            }
        }
        return SR_INVALID;
    }

    case SR_V_NUMBER:
        if (value[0] == '-' || value[0] == '+')
            return sr_made_of(value + 1, 11, FALSE, TRUE, NULL)
                   ? value : SR_INVALID;
        if (value[0] == '$')
            return sr_made_of(value + 1, 8, FALSE, TRUE, "abcdefABCDEF")
                   ? value : SR_INVALID;
        if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
            return sr_made_of(value + 2, 8, FALSE, TRUE, "abcdefABCDEF")
                   ? value : SR_INVALID;
        return sr_made_of(value, 11, FALSE, TRUE, NULL) ? value : SR_INVALID;

    case SR_V_DRIVER:
        return sr_made_of(value, 64, TRUE, TRUE, "._-:/") ? value : SR_INVALID;

    case SR_V_WORD:
        return sr_made_of(value, 16, TRUE, TRUE, "_-") ? value : SR_INVALID;

    case SR_V_ADDRESS:
        return sr_made_of(value, 64, TRUE, TRUE, ".:/%-") ? value : SR_INVALID;

    case SR_V_HOSTNAME:
        return sr_made_of(value, 64, TRUE, TRUE, ".-_") ? value : SR_INVALID;

    default:
        break;
    }

    return SR_INVALID;
}

ULONG sr_interface_text(SrOut *o, const char *ifname, char *text,
                        char *device, ULONG devicelen)
{
    char  name[40];
    char  key[SR_KEY_MAX];
    char *cursor = text;
    char *line;
    ULONG before;
    ULONG omitted = 0;

    if (o == NULL)
        return 0;

    before = o->lines;

    if (device != NULL && devicelen > 0UL)
        device[0] = '\0';

    sr_key_part(name, sizeof(name), ifname);

    if (text == NULL)
    {
        sr_key(key, sizeof(key), "config", name, "file");
        sr_str(o, key, SR_UNAVAILABLE);
        return o->lines - before;
    }

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *keyword;
        char *value;

        ami_cfg_strip_comment(line, "#;");
        line = ami_cfg_trim(line);
        if (*line == '\0')
            continue;

        pos = line;
        while (ami_cfg_next_pair(&pos, &keyword, &value))
        {
            const SrIfKey *k = sr_if_key(keyword);
            const char    *shown;

            if (k == NULL)
            {
                omitted++;
                continue;
            }

            if (k->cls == SR_ADDRESSES && !o->addresses)
                continue;

            shown = sr_shape(k, value);

            sr_key(key, sizeof(key), "config", name, k->key);
            sr_str(o, key, shown);

            if (device != NULL && devicelen > 0UL &&
                k->shape == SR_V_DRIVER && shown == value)
            {
                const char *base = value;
                const char *p;

                for (p = value; *p != '\0'; p++)
                {
                    if (*p == '/' || *p == ':')
                        base = p + 1;
                }
                ami_cfg_copy_string(device, devicelen, base);
            }
        }
    }

    sr_key(key, sizeof(key), "config", name, "omitted");
    sr_ulong(o, key, omitted);

    return o->lines - before;
}

/* ------------------------------------------------------ the probe record --- */

/*
 * Codes whose value is a station address, part of one, or a card's serial
 * number.  ANXDIAG_PC_NODEID is not here: its value is a flag.
 */
static const UWORD sr_diag_identifying[] =
{
    ANXDIAG_MAC_HI, ANXDIAG_MAC_LO, ANXDIAG_PNP_SERIAL, ANXDIAG_NE_NODEID_PORT
};

/* Every other code anxdiag.h defines.  A code missing from both tables is one
   this build has never heard of, and its value is not printed. */
static const UWORD sr_diag_plain[] =
{
    ANXDIAG_START, ANXDIAG_EXPANSION, ANXDIAG_BOARDS, ANXDIAG_NOMATCH,
    ANXDIAG_UNITS_FULL, ANXDIAG_DONE,
    ANXDIAG_ZORRO_FOUND, ANXDIAG_FIXED_TRY, ANXDIAG_NO_CORE,
    ANXDIAG_ATTACH_OK, ANXDIAG_ATTACH_FAIL, ANXDIAG_MAC_SOURCE,
    ANXDIAG_GETODD, ANXDIAG_UNIT, ANXDIAG_CR_READ, ANXDIAG_ODD_RETRY,
    ANXDIAG_CHIP, ANXDIAG_EL3_MFG, ANXDIAG_EL3_ORDER, ANXDIAG_EL3_MEDIA,
    ANXDIAG_ODD_PLAIN, ANXDIAG_ODD_WORD, ANXDIAG_ODDWIN, ANXDIAG_BUF_SEEN,
    ANXDIAG_PC_RESOURCE, ANXDIAG_PC_OWN, ANXDIAG_PC_FUNCID,
    ANXDIAG_PC_NOTLAN, ANXDIAG_PC_MANFID, ANXDIAG_PC_FUNCE,
    ANXDIAG_PC_NODEID, ANXDIAG_PC_NOCONFIG, ANXDIAG_PC_CFGBASE,
    ANXDIAG_PC_NOCFTABLE, ANXDIAG_PC_INDEX, ANXDIAG_PC_IOMODE,
    ANXDIAG_PC_COR, ANXDIAG_PC_CR, ANXDIAG_PC_COR2, ANXDIAG_PC_CR2,
    ANXDIAG_PC_SILENT, ANXDIAG_PC_IRQMODE, ANXDIAG_PC_IRQSKIP,
    ANXDIAG_PC_CLAIMED, ANXDIAG_PC_CARD, ANXDIAG_PC_CFTABLE,
    ANXDIAG_PC_MISC, ANXDIAG_PC_CORVAL, ANXDIAG_PC_SETTLE,
    ANXDIAG_PC_NOROW, ANXDIAG_PC_RESET, ANXDIAG_PC_CFCOUNT,
    ANXDIAG_PC_CFPICK, ANXDIAG_PC_IOWIN, ANXDIAG_PC_IOOFF, ANXDIAG_PC_MFC,
    ANXDIAG_PC_MFCFUNC, ANXDIAG_PC_MFCNOLAN, ANXDIAG_PC_MFCIOBASE,
    ANXDIAG_EL3_FIFO, ANXDIAG_CR_RETRY, ANXDIAG_CLOCK, ANXDIAG_CLOCK_LINE,
    ANXDIAG_PNP_VENDOR, ANXDIAG_PNP_CSUM, ANXDIAG_PNP_IO,
    ANXDIAG_PNP_SETTLE, ANXDIAG_PNP_CR, ANXDIAG_PNP_SILENT, ANXDIAG_PNP_OK,
    ANXDIAG_DTREE_FOUND, ANXDIAG_GENET_REV, ANXDIAG_GENET_MEM,
    ANXDIAG_GENET_IRQ, ANXDIAG_GENET_PHY, ANXDIAG_GENET_DMA,
    ANXDIAG_GENET_GIC, ANXDIAG_CACHE_GUARD, ANXDIAG_CACHE_WHY
};

LONG sr_anxdiag_value_class(UWORD code)
{
    ULONG i;

    for (i = 0; i < sizeof(sr_diag_identifying) / sizeof(sr_diag_identifying[0]); i++)
    {
        if (sr_diag_identifying[i] == code)
            return 0;
    }
    for (i = 0; i < sizeof(sr_diag_plain) / sizeof(sr_diag_plain[0]); i++)
    {
        if (sr_diag_plain[i] == code)
            return 1;
    }
    return -1;
}
