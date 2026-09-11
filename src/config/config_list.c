/*
 * AmiNetXDuo, the list of interface DESCRIPTIONS: how it grows, how it is
 * ordered, and who gives it back.
 *
 * SPLIT OUT OF config_file.c so that it can be tested on the host.  Everything
 * here works on memory buffers and on the two hooks config_file.c provides --
 * ami_cfg_read_file() for a file's bytes and ami_cfg_scan_interfaces() for the
 * names in the drawer -- so test/test_config.c drives this exact code with a
 * fixture table instead of DEVS:NetInterfaces.  That matters more here than it
 * does for the parsers: the defect this file exists to prevent was a config
 * that quietly held fewer interfaces than the drawer described, and a test
 * that cannot enumerate a drawer of five cannot catch it coming back.
 *
 * THE RULE THIS FILE KEEPS: a description is not an attachment.  Any number of
 * interfaces may be described and every one of them is read.  How many may be
 * ONLINE AT ONCE is a different question with a different answer, refused in
 * src/netstack at the attach.  See the head of include/aminetxduo/config.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"

#include "aminetxduo/config_advice.h"
#include "aminetxduo/compat.h"

/* -------------------------------------------------------------- utilities */

static VOID join_path(char *dst, ULONG dstlen, const char *dir, const char *name)
{
    ULONG pos;

    ami_cfg_copy_string(dst, dstlen, dir);
    pos = ami_cfg_strlen(dst);

    if (pos > 0 && dst[pos - 1] != '/' && dst[pos - 1] != ':' && pos + 1 < dstlen)
    {
        dst[pos++] = '/';
        dst[pos]   = '\0';
    }

    ami_cfg_copy_string(dst + pos, dstlen - pos, name);
}

/* ---------------------------------------------------------------- paths -- */

BOOL ami_cfg_has_path(const char *name)
{
    const char *p;

    if (name == NULL)
        return FALSE;

    for (p = name; *p != '\0'; p++)
    {
        if (*p == ':' || *p == '/')
            return TRUE;
    }

    return FALSE;
}

const char *ami_cfg_file_part(const char *name)
{
    const char *last;
    const char *p;

    if (name == NULL)
        return NULL;

    last = name;
    for (p = name; *p != '\0'; p++)
    {
        if (*p == ':' || *p == '/')
            last = p + 1;
    }

    return last;
}

/* ------------------------------------------------------------- one file -- */

LONG ami_config_load_interface(const char *name, AmiIfConfig *out)
{
    char  path[AMI_CFG_PATH_LEN + AMI_CFG_NAME_LEN + 8];
    char *buf;
    LONG  result;

    if (name == NULL || out == NULL)
        return AMI_CFG_ERR_SYNTAX;

    /*
     * WHERE AN INTERFACE FILE IS LOOKED FOR, and it is not one place.
     *
     * Roadshow and AmiTCP_NG both take a path as a path: the file named on the
     * command line, then DEVS:NetInterfaces, then SYS:Storage/NetInterfaces.
     * This joined DEVS:NetInterfaces to whatever it was given and looked
     * nowhere else, so `AddNetInterface Work:mycfg' quietly read
     * DEVS:NetInterfaces/Work:mycfg -- or, once the caller had reduced the
     * argument to its basename, a DIFFERENT FILE with the same name.  A user
     * migrating a working script gets the wrong interface or none.
     *
     * The interface's NAME is still its basename, whichever file supplied it:
     * that is what RemoveNetInterface, Online and ShowNetStatus will be given.
     * The two drawers are searched only for a BARE name; see below.
     */
    buf = NULL;

    if (ami_cfg_has_path(name))
    {
        /*
         * A PATH IS NOT A HINT.  A name carrying a device or a directory names
         * ONE file, and if that file is not there the answer is "not there" --
         * never the drawer's file that happens to share the basename.  Falling
         * back would turn `AddNetInterface Work:weth0' with a typo in it into
         * a DIFFERENT interface coming up, reported as success, which is the
         * failure this whole search order exists to remove.
         */
        buf = (char *)ami_cfg_read_file(name, NULL);
    }
    else
    {
        join_path(path, sizeof(path), AMI_CFG_DIR_NETINTERFACES, name);
        buf = (char *)ami_cfg_read_file(path, NULL);

        if (buf == NULL)
        {
            join_path(path, sizeof(path), AMI_CFG_DIR_STORAGE_NETINTERFACES,
                      name);
            buf = (char *)ami_cfg_read_file(path, NULL);
        }
    }

    if (buf == NULL)
    {
        ami_cfg_zero(out, sizeof(*out));
        return AMI_CFG_ERR_IO;
    }

    if (ami_cfg_has_path(name))
        ami_cfg_copy_string(path, sizeof(path), name);

    /*
     * `path` is on this stack frame and the reporter is handed it by pointer,
     * so the name is cleared before this function returns rather than left for
     * the next caller to overwrite.
     */
    ami_cfg_problem_file(path);
    result = ami_cfg_parse_interface(ami_cfg_file_part(name), buf, out);
    ami_cfg_problem_file(NULL);

    ami_free(buf);

    return result;
}

/* ------------------------------------------------------------ the list --- */

/*
 * Grow interfaces[] until it can hold `want`.
 *
 * Doubling from AMI_CFG_IFACE_FLOOR, so a drawer of three costs one growth and
 * a drawer of nine costs three, and the common machine -- one card, one file --
 * pays a single allocation for the floor and never grows at all.  There is no
 * realloc() here (aminetxduo/compat.h is ami_alloc/ami_free over AllocVec), so
 * a growth is allocate, copy, free, which is also what makes it safe to fail:
 * the old list is still intact when the new one could not be had.
 *
 * A GROWTH MOVES THE LIST, and nx_ip_interface_attach() keeps the interface's
 * name POINTER rather than the name (src/netstack/netstack.c).  So growing
 * while an interface is attached would leave NetX Duo naming freed memory.
 * Every growth in the tree is therefore before any attach: the parser appends
 * as it reads the drawer, and ami_config_load() takes AMI_CFG_IFACE_FLOOR up
 * front and fails outright if it cannot, which is what makes the netstack's
 * own reserve -- for a slot index it was handed, always below the floor -- a
 * call that can never move anything.
 */
BOOL ami_config_reserve(AmiConfig *cfg, UWORD want)
{
    AmiIfConfig *grown;
    ULONG        capacity;

    if (cfg == NULL)
        return FALSE;

    if (want <= cfg->interface_capacity && cfg->interfaces != NULL)
        return TRUE;

    capacity = (ULONG)((cfg->interface_capacity > 0)
                       ? cfg->interface_capacity
                       : (UWORD)AMI_CFG_IFACE_FLOOR);
    while (capacity < (ULONG)want)
        capacity *= 2UL;

    /* UWORD holds the count, so the list cannot usefully pass 65535 entries;
       a drawer that large is a filesystem fault rather than a configuration. */
    if (capacity > 0xFFFFUL)
        return FALSE;

    grown = (AmiIfConfig *)ami_alloc((ULONG)(capacity * sizeof(AmiIfConfig)));
    if (grown == NULL)
        return FALSE;

    ami_cfg_zero(grown, (ULONG)(capacity * sizeof(AmiIfConfig)));

    if (cfg->interfaces != NULL)
    {
        UWORD i;

        for (i = 0; i < cfg->interface_count; i++)
            grown[i] = cfg->interfaces[i];

        ami_free(cfg->interfaces);
    }

    cfg->interfaces         = grown;
    cfg->interface_capacity = (UWORD)capacity;

    return TRUE;
}

VOID ami_config_free(AmiConfig *cfg)
{
    if (cfg == NULL)
        return;

    if (cfg->interfaces != NULL)
        ami_free(cfg->interfaces);

    cfg->interfaces         = NULL;
    cfg->interface_capacity = 0;
    cfg->interface_count    = 0;
}

/*
 * Roadshow processes the interface files in alphabetical order (a PRI tooltype
 * can override that, and icons are not read here). A sorted list makes the
 * first interface deterministic, and its gateway becomes the default route
 * when no routes file exists.
 *
 * THE ORDER IS NOW ONLY AN ORDER.  It used to be a survival rank, because the
 * list stopped at two and the sort ran before the ceiling was applied, so the
 * alphabet silently decided which definitions existed and a card renamed from
 * "eth0" to "wifi" could vanish without a word.  Nothing is dropped any more,
 * so the sort settles precedence -- which interface is first, and whose
 * gateway becomes the default route -- and nothing else.
 */
static VOID insert_interface(AmiConfig *cfg, const AmiIfConfig *iface)
{
    UWORD pos;
    UWORD i;

    /*
     * NO CEILING, and no message about the list being full.  There used to be
     * both: the list stopped at two and the file that would not fit was
     * dropped behind an AMI_WARN that no shipped build compiles, so the
     * interface simply stopped existing and nothing on the machine said so.
     * A description costs a struct on disk and nothing on the wire, so the
     * only honest answer to a third interface file is to read it; the limit
     * that is real belongs at the attach, where src/netstack refuses by name.
     * Telling a user to delete a file they wrote so that ours fits is that
     * same defect with a bigger constant in front of it.
     *
     * The only failure left is running out of memory, which is not a limit on
     * interfaces and is reported as what it is.
     */
    if (!ami_config_reserve(cfg, (UWORD)(cfg->interface_count + 1U)))
    {
        char text[AMI_CFG_NAME_LEN + 96];

        ami_cfg_problem_file(AMI_CFG_DIR_NETINTERFACES);
        ami_cfg_join3(text, sizeof(text), "there was not enough memory to "
                      "read '", iface->name, "', so that interface is missing");
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR, text, AMI_CFG_ADVICE_THIS_IS_MEMORY_NOT);
        ami_cfg_problem_file(NULL);
        return;
    }

    for (pos = 0; pos < cfg->interface_count; pos++)
    {
        if (ami_cfg_stricmp(iface->name, cfg->interfaces[pos].name) < 0)
            break;
    }

    for (i = cfg->interface_count; i > pos; i--)
        cfg->interfaces[i] = cfg->interfaces[i - 1];

    cfg->interfaces[pos] = *iface;
    cfg->interface_count++;
}

/*
 * One name from the drawer.  This is the sink ami_cfg_scan_interfaces() calls,
 * and the only place a file's contents become a description.
 *
 * A file that will not parse is REPORTED and skipped, which is not the same
 * thing as the truncation this rework removed: the user is told, by name, that
 * the file is unusable, and the reason was printed above it by the parser.
 */
VOID ami_cfg_take_interface(AmiConfig *cfg, const char *name)
{
    AmiIfConfig iface;

    if (cfg == NULL || name == NULL)
        return;

    if (ami_config_load_interface(name, &iface) != AMI_CFG_OK)
    {
        char text[AMI_CFG_NAME_LEN + 64];

        ami_cfg_problem_file(AMI_CFG_DIR_NETINTERFACES);
        ami_cfg_join3(text, sizeof(text), "the file '", name,
                      "' cannot be used, so that interface does not exist");
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR, text, AMI_CFG_ADVICE_THE_PROBLEMS_LISTED_ABOVE);
        ami_cfg_problem_file(NULL);
        return;
    }

    AMI_INFO("config: interface %s: %s unit %lu",
             iface.name, iface.device, (unsigned long)iface.unit);

    insert_interface(cfg, &iface);
}

/*
 * Every interface file in the drawer, however many there are.
 *
 * The enumeration itself is ami_cfg_scan_interfaces(), in config_file.c,
 * because it is the one part of this that has to talk to AmigaDOS.  It returns
 * FALSE only when the drawer is missing, which has its own message; an empty
 * drawer scans successfully and is caught by the count below.
 */
/* ------------------------------------------------- resolver, from a card -- */

/*
 * NAMESERVER AND DOMAIN IN AN INTERFACE FILE.
 *
 * AmiTCP_NG's installer writes them there.  Roadshow keeps them in
 * DEVS:Internet/name_resolution and this tree reads that file and the netdb
 * file -- and nothing else, so a machine migrated from AmiTCP_NG came up with
 * NO NAME SERVER and was told nothing about it: the keywords parse, and the
 * interface parser had them marked as handled elsewhere when they were handled
 * nowhere.
 *
 * They are read here as the LAST source, so an installation that has a
 * name_resolution file is unchanged.  A note says where the value came from
 * and where it belongs, because a setting that works only because a fallback
 * found it is one file edit away from failing silently.
 */
static VOID resolver_from_one(AmiConfig *cfg, const char *name)
{
    char  path[AMI_CFG_PATH_LEN + AMI_CFG_NAME_LEN + 8];
    char *buf;
    char *cursor;
    char *line;
    BOOL  took_server = FALSE;
    BOOL  want_server;

    if (cfg == NULL || name == NULL)
        return;

    if (cfg->resolver.nameserver_count != 0 && cfg->resolver.domain[0] != '\0')
        return;

    /* Only what is MISSING.  A resolver that already has a name server keeps
       exactly the ones it was configured with: appending a second from here
       would change which server answers, on a machine whose name_resolution
       file is right. */
    want_server = (BOOL)(cfg->resolver.nameserver_count == 0);

    join_path(path, sizeof(path), AMI_CFG_DIR_NETINTERFACES, name);
    buf = (char *)ami_cfg_read_file(path, NULL);
    if (buf == NULL)
        return;

    /*
     * Two keywords, read by hand.
     *
     * ami_cfg_parse_resolver() would have done it in one call and was the
     * first shape of this -- but it REPORTS the keywords it does not know,
     * so every interface file grew four complaints that DEVICE, UNIT,
     * CONFIGURE and STATE are unknown and that "this file holds NAMESERVER,
     * DOMAIN and SEARCH lines".  They are not unknown here; they are the
     * interface parser's, and this pass is a guest in its file.
     */
    cursor = buf;
    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *key;
        char *value;

        ami_cfg_strip_comment(line, "#;");
        line = ami_cfg_trim(line);
        if (*line == '\0')
            continue;

        pos = line;
        if (!ami_cfg_next_pair(&pos, &key, &value))
            continue;

        if (ami_cfg_stricmp(key, "nameserver") == 0)
        {
            ULONG addr = 0;

            if (!want_server ||
                cfg->resolver.nameserver_count >= AMI_CFG_MAX_NAMESERVERS)
                continue;

            /* A bad one IS reported: it was written to be used. */
            if (!ami_config_parse_ip(value, &addr))
            {
                ami_cfg_problem_file(path);
                ami_cfg_problem_code(0, AMI_CFG_PROBLEM_ERROR,
                                     AMI_CFG_SAYS_NAMESERVER_IN_AN_INTERFACE,
                                     AMI_CFG_ADVICE_A_NAME_SERVER_IS);
                ami_cfg_problem_file(NULL);
                continue;
            }

            /* Negative: statically configured, one reference. */
            cfg->resolver.nameserver_use[cfg->resolver.nameserver_count] = -1;
            cfg->resolver.nameserver[cfg->resolver.nameserver_count]     = addr;
            cfg->resolver.nameserver_count++;
            took_server = TRUE;
        }
        else if (ami_cfg_stricmp(key, "domain") == 0)
        {
            if (cfg->resolver.domain[0] == '\0')
                ami_cfg_copy_string(cfg->resolver.domain,
                                    sizeof(cfg->resolver.domain), value);
        }
    }

    ami_free(buf);

    if (took_server)
    {
        ami_cfg_problem_file(path);
        ami_cfg_problem_code(0, AMI_CFG_PROBLEM_NOTE,
                             AMI_CFG_SAYS_NAMESERVER_IN_AN_INTERFACE,
                             AMI_CFG_ADVICE_PUT_IT_IN_NAME_RESOLUTION);
        ami_cfg_problem_file(NULL);
    }
}

VOID ami_config_resolver_from_interfaces(AmiConfig *cfg)
{
    if (cfg == NULL)
        return;

    if (cfg->resolver.nameserver_count != 0 && cfg->resolver.domain[0] != '\0')
        return;

    (VOID)ami_cfg_scan_interfaces(cfg, resolver_from_one);
}

VOID ami_config_load_interfaces(AmiConfig *cfg)
{
    if (cfg == NULL)
        return;

    if (!ami_cfg_scan_interfaces(cfg, ami_cfg_take_interface))
        return;

    if (cfg->interface_count == 0)
    {
        ami_cfg_problem_file(AMI_CFG_DIR_NETINTERFACES);
        ami_cfg_problem_code(0, AMI_CFG_PROBLEM_ERROR, AMI_CFG_SAYS_THE_DEVS_NETINTERFACES_DRAWER, AMI_CFG_ADVICE_ONE_FILE_PER_NETWORK);
        ami_cfg_problem_file(NULL);
    }
}
