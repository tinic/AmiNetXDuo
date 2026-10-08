/*
 * AmiNetXDuo, picking this machine's name out of the places that offer one.
 *
 * Four sources, ranked by AmiHostnameSource (aminetxduo/config.h). Nothing
 * here does file access: the callers read the files and hand the text over, so
 * the host test drives the same chain the Amiga does.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"

#include <stddef.h>
#include "aminetxduo/compat.h"

/* RFC 1123 2.1 relaxes RFC 952 to allow a leading digit. The rest stands. */
#define AMI_CFG_LABEL_MAX   63

static BOOL label_valid(const char *s, ULONG len)
{
    ULONG i;

    if (len == 0 || len > AMI_CFG_LABEL_MAX)
        return FALSE;
    if (s[0] == '-' || s[len - 1] == '-')
        return FALSE;

    for (i = 0; i < len; i++)
    {
        char c = s[i];

        if (c >= 'a' && c <= 'z')
            continue;
        if (c >= 'A' && c <= 'Z')
            continue;
        if (c >= '0' && c <= '9')
            continue;
        if (c == '-')
            continue;

        return FALSE;
    }

    return TRUE;
}

BOOL ami_config_hostname_valid(const char *name)
{
    ULONG start = 0;
    ULONG i;

    if (name == NULL || name[0] == '\0')
        return FALSE;

    /* A name that does not fit cannot be stored. */
    if (ami_cfg_strlen(name) >= (ULONG)AMI_CFG_NAME_LEN)
        return FALSE;

    for (i = 0; ; i++)
    {
        if (name[i] != '.' && name[i] != '\0')
            continue;

        if (!label_valid(name + start, i - start))
            return FALSE;

        if (name[i] == '\0')
            break;

        start = i + 1;
    }

    return TRUE;
}

/*
 * The default name of a machine that has none.
 *
 * "amiga" alone was the old default, so every unconfigured machine on a
 * segment claimed amiga.local and the loser of the probe lost its name. The
 * last three octets of the hardware address are the part a card maker varies,
 * so 00:80:10:49:00:07 gives "amiga-490007": a valid RFC 1123 label, the same
 * on every boot of the same card, and short enough to type. Lower case because
 * mDNS compares without case (RFC 6762 16).
 *
 * Three octets, not six: the first three are the OUI, identical across every
 * card of one make, so they cost nine characters and separate nothing. Two
 * cards of different makes can still share a tail, which RFC 6762 9 probing
 * settles.
 *
 * FALSE when there is no address to derive from, which leaves the caller its
 * old fallback. A name that moves between boots is worse than one that
 * collides.
 */
BOOL ami_config_hostname_from_hwaddr(const UBYTE *hw, ULONG hwlen,
                                     char *out, ULONG size)
{
    static const char digits[] = "0123456789abcdef";
    static const char prefix[] = "amiga-";
    const ULONG       taken    = 3;
    ULONG             i;
    ULONG             at;
    BOOL              any = FALSE;

    if (out == NULL || size < sizeof(prefix) + taken * 2)
        return FALSE;
    if (hw == NULL || hwlen < taken)
        return FALSE;

    /* An all-zero address is what a device that has none reports, and what
       ami_sana2_get_mac() writes for an interface that is not there. */
    for (i = 0; i < hwlen; i++)
    {
        if (hw[i] != 0)
            any = TRUE;
    }

    if (!any)
        return FALSE;

    for (at = 0; at < sizeof(prefix) - 1; at++)
        out[at] = prefix[at];

    for (i = hwlen - taken; i < hwlen; i++)
    {
        out[at++] = digits[(hw[i] >> 4) & 0x0F];
        out[at++] = digits[hw[i] & 0x0F];
    }

    out[at] = '\0';

    return TRUE;
}

const char *ami_config_hostname_source_text(UWORD source)
{
#define HOSTNAME_SOURCES(X) \
    X(NAMERES,   "name_resolution") \
    X(HOSTS,     "hosts") \
    X(DHCP,      "DHCP") \
    X(INTERFACE, "interface ID") \
    X(ENV,       "ENV:HOSTNAME")
    struct HostnameSourceNames
    {
#define SOURCE_NAME(code, text) char name_##code[sizeof(text)];
        HOSTNAME_SOURCES(SOURCE_NAME)
#undef SOURCE_NAME
    };
    static const struct HostnameSourceNames pool =
    {
#define SOURCE_TEXT(code, text) text,
        HOSTNAME_SOURCES(SOURCE_TEXT)
#undef SOURCE_TEXT
    };
    static const struct
    {
        UBYTE source;
        UBYTE offset;
    } names[] =
    {
#define SOURCE_ENTRY(code, text) \
        { AMI_HOSTNAME_##code, (UBYTE)offsetof(struct HostnameSourceNames, name_##code) },
        HOSTNAME_SOURCES(SOURCE_ENTRY)
#undef SOURCE_ENTRY
    };
    UWORD i;

    _Static_assert(sizeof(pool) <= 256UL, "hostname source offsets must fit in UBYTE");
#define SOURCE_ID(code, text) \
    _Static_assert(AMI_HOSTNAME_##code >= 0 && AMI_HOSTNAME_##code <= 255, \
                   "hostname source IDs must fit in UBYTE");
    HOSTNAME_SOURCES(SOURCE_ID)
#undef SOURCE_ID
#undef HOSTNAME_SOURCES

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        /* Keep the full UWORD comparison for unknown source IDs. */
        if (names[i].source == source)
            return (const char *)&pool + names[i].offset;
    }

    return NULL;
}

UWORD ami_config_hostname_rank(UWORD source)
{
    /* The hosts file's HOSTNAME= is read only when name_resolution named
       nothing, and holds the place name_resolution would have. */
    if (source == (UWORD)AMI_HOSTNAME_HOSTS)
        return (UWORD)AMI_HOSTNAME_NAMERES;

    return source;
}

/* TRUE for a source whose text was never required to be a host name. */
static BOOL source_is_checked(UWORD source)
{
    return (BOOL)(source == (UWORD)AMI_HOSTNAME_INTERFACE ||
                  source == (UWORD)AMI_HOSTNAME_DHCP);
}

BOOL ami_config_hostname_offer(AmiConfig *cfg, UWORD source, const char *name)
{
    if (cfg == NULL || name == NULL || name[0] == '\0')
        return FALSE;
    if (source == (UWORD)AMI_HOSTNAME_NONE ||
        ami_config_hostname_rank(source) <
            ami_config_hostname_rank(cfg->hostname_source))
        return FALSE;
    if (source_is_checked(source) && !ami_config_hostname_valid(name))
        return FALSE;

    ami_cfg_copy_string(cfg->hostname, sizeof(cfg->hostname), name);
    cfg->hostname_source = source;

    return TRUE;
}

VOID ami_cfg_hostname_from_files(AmiConfig *cfg, char *env_text)
{
    UWORD i;

    if (cfg == NULL)
        return;

    /*
     * Weakest first, so the rank check in the offer decides. Interfaces are in
     * name order, so the first usable ID= is the same one on every boot. An ID
     * that is not a host name is refused, and the next source answers instead.
     */
    for (i = 0; i < cfg->interface_count; i++)
    {
        if (ami_config_hostname_offer(cfg, (UWORD)AMI_HOSTNAME_INTERFACE,
                                      cfg->interfaces[i].id))
            break;
    }

    if (env_text != NULL)
    {
        char *cursor = env_text;
        char *line   = ami_cfg_next_line(&cursor);

        if (line != NULL)
            (VOID)ami_config_hostname_offer(cfg, (UWORD)AMI_HOSTNAME_ENV,
                                            ami_cfg_trim(line));
    }
}
