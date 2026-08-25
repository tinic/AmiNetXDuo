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

/* ------------------------------------------------------------- one file -- */

LONG ami_config_load_interface(const char *name, AmiIfConfig *out)
{
    char  path[AMI_CFG_PATH_LEN + AMI_CFG_NAME_LEN + 8];
    char *buf;
    LONG  result;

    if (name == NULL || out == NULL)
        return AMI_CFG_ERR_SYNTAX;

    join_path(path, sizeof(path), AMI_CFG_DIR_NETINTERFACES, name);

    buf = (char *)ami_cfg_read_file(path, NULL);
    if (buf == NULL)
    {
        ami_cfg_zero(out, sizeof(*out));
        return AMI_CFG_ERR_IO;
    }

    /*
     * `path` is on this stack frame and the reporter is handed it by pointer,
     * so the name is cleared before this function returns rather than left for
     * the next caller to overwrite.
     */
    ami_cfg_problem_file(path);
    result = ami_cfg_parse_interface(name, buf, out);
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
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR, text,
                        "This is memory, not a limit on how many interfaces "
                        "may be described.  Close a program and try again.");
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
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR, text,
                        "The problems listed above it say why.  "
                        "NetSetup can rewrite the file from scratch.");
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
VOID ami_config_load_interfaces(AmiConfig *cfg)
{
    if (cfg == NULL)
        return;

    if (!ami_cfg_scan_interfaces(cfg, ami_cfg_take_interface))
        return;

    if (cfg->interface_count == 0)
    {
        ami_cfg_problem_file(AMI_CFG_DIR_NETINTERFACES);
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR,
                        "the DEVS:NetInterfaces drawer holds no usable "
                        "interface file",
                        "One file per network card goes in there.  The name "
                        "of the file is the name of the card, and eth0 is "
                        "the usual choice.  NetSetup writes one.");
        ami_cfg_problem_file(NULL);
    }
}
