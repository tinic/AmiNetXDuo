/*
 * AmiNetXDuo -- picking this machine's name out of the places that offer one.
 *
 * Four sources, ranked by AmiHostnameSource (aminetxduo/config.h). Everything
 * here is pure: the callers read the files and hand the text over, so the host
 * test drives the same chain the Amiga does.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

/* RFC 1123 2.1 relaxes RFC 952 to allow a leading digit; the rest stands. */
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

    /* A name that does not fit is not a name we could store. */
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

const char *ami_config_hostname_source_text(UWORD source)
{
    static const struct
    {
        UWORD       source;
        const char *text;
    }
    names[] =
    {
        { (UWORD)AMI_HOSTNAME_NAMERES,   "name_resolution" },
        { (UWORD)AMI_HOSTNAME_DHCP,      "DHCP"            },
        { (UWORD)AMI_HOSTNAME_INTERFACE, "interface ID"    },
        { (UWORD)AMI_HOSTNAME_ENV,       "ENV:HOSTNAME"    },
        { (UWORD)AMI_HOSTNAME_NONE,      NULL              }
    };
    UWORD i;

    for (i = 0; names[i].text != NULL; i++)
    {
        if (names[i].source == source)
            return names[i].text;
    }

    return NULL;
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
    if (source == (UWORD)AMI_HOSTNAME_NONE || source < cfg->hostname_source)
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
     * Weakest first, so the offer's rank check does the deciding. Interfaces
     * are in name order, so the first usable ID= is the same one on every
     * boot; an ID that is not a host name is refused and the next source
     * answers instead.
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
