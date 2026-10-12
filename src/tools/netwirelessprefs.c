/*
 * NetWirelessPrefs, the Wi-Fi window: the networks in range and joining
 * one, written to WirelessManager's ENV:Sys/Wireless.prefs.  From a Shell,
 * LIST and STATUS print what the window shows; SETUP is the window the
 * installer runs, and SETUP TO <file> writes that file alone.
 *
 * WirelessManager owns the association: this program scans and reads the
 * link, writes the file and restarts WirelessManager, and never joins or
 * sets a key itself.  A joined network is stored as psk=<64 hex>, the key
 * derived from passphrase and SSID, so the passphrase is never on disk.
 *
 * Laid out in the screen's font, topaz 8 when that does not fit, by the
 * host-tested netwireless_layout.c.  GadTools on Kickstart 2.04; scanning
 * needs exec V39's memory pools.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netwireless_dev.h"
#include "netwireless_key.h"
#include "netwireless_layout.h"
#include "netwireless_prefs.h"
#include "netwireless_sig.h"
#include "netwireless_wm.h"
#include "aminetxduo/version.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <intuition/sghooks.h>
#include <libraries/gadtools.h>
#include <utility/hooks.h>
#include <workbench/icon.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <proto/intuition.h>

const char *const tool_name = "NetWirelessPrefs";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetWirelessPrefs");

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;
struct Library       *IconBase;

#define TEMPLATE "DEVICE/K,UNIT/K/N,INTERFACE/K,WM/K,LIST/S,STATUS/S,SETUP/S," \
                 "JOIN/K,PASSPHRASE/K,HIDDEN/S,TO/K,DISCONNECT/S"

enum
{
    ARG_DEVICE,
    ARG_UNIT,
    ARG_INTERFACE,
    ARG_WM,
    ARG_LIST,
    ARG_STATUS,
    ARG_SETUP,
    ARG_JOIN,
    ARG_PASSPHRASE,
    ARG_HIDDEN,
    ARG_TO,
    ARG_DISCONNECT,
    ARG_COUNT
};

/* SETUP's answers to the installer: 5 and above mean "ask in text". */
#define NW_SETUP_WRITTEN    RETURN_OK
#define NW_SETUP_CANCELLED  RETURN_WARN
#define NW_SETUP_NO_DEVICE  RETURN_ERROR

#define NW_LIST_MAX     32
#define NW_NETS_MAX     16          /* network={} blocks edited          */
#define NW_DEVICE_LEN   128
#define NW_ROW_LEN      80
#define NW_PASS_MAX     64
#define NW_FILE_MAX     (32UL * 1024UL)
#define NW_JOIN_WAIT    30          /* seconds for the association      */
#define NW_REFRESH      5           /* seconds between status readings  */

static const char nw_prefs_env[]    = "ENV:Sys/Wireless.prefs";
static const char nw_prefs_envarc[] = "ENVARC:Sys/Wireless.prefs";

enum { MENU_SCAN = 1, MENU_JOIN, MENU_KNOWN, MENU_SNAPSHOT, MENU_QUIT };

static struct NewMenu nw_menu[] =
{
    { NM_TITLE, (STRPTR)"Project",   NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Scan",      (STRPTR)"S", 0, 0, (APTR)MENU_SCAN },
    { NM_ITEM,  (STRPTR)"Join...",   (STRPTR)"J", 0, 0, (APTR)MENU_JOIN },
    { NM_ITEM,  (STRPTR)"Known networks...", (STRPTR)"K", 0, 0, (APTR)MENU_KNOWN },
    { NM_ITEM,  NM_BARLABEL,         NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Snapshot",  NULL,        0, 0, (APTR)MENU_SNAPSHOT },
    { NM_ITEM,  NM_BARLABEL,         NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",      (STRPTR)"Q", 0, 0, (APTR)MENU_QUIT },
    { NM_END,   NULL,                NULL,        0, 0, NULL }
};

typedef struct NwWin
{
    struct Window *win;
    struct Gadget *glist;
} NwWin;

static struct
{
    /* the unit */
    NwDev           dev;
    BOOL            dev_open;
    char            device[NW_DEVICE_LEN];
    ULONG           unit;
    char            wm[NW_DEVICE_LEN];
    BOOL            setup;
    char            to[256];                /* SETUP TO: this file alone */
    NwScan          nets[NW_LIST_MAX];
    LONG            count;

    /* the screen and the windows */
    struct Screen  *screen;
    APTR            visual;
    struct TextAttr *ta;
    struct TextFont *font;
    NwLayout        lay;
    struct Menu    *menu;
    NwWin           main, join, known;
    struct Gadget  *g[NW_L_COUNT];
    struct List     rows;
    struct Node     row[NW_LIST_MAX];
    char            row_text[NW_LIST_MAX][NW_ROW_LEN];
    char            net_text[NW_SSID_MAX + 16];
    char            sig_text[56];
    char            status[80];
    LONG            selected;
    UBYTE           sel_ssid[NW_SSID_MAX];  /* the selection, by name      */
    LONG            sel_len;
    int             shown_bars;             /* the Signal line's, held     */
    ULONG           click_secs, click_micros;
    WORD            want_left, want_top;
    struct WBStartup *wbs;

    /* the join */
    char            pass[NW_PASS_MAX + 1];
    int             pass_len;
    BOOL            pass_shown;
    BOOL            open_network;
    UBYTE           join_ssid[NW_SSID_MAX];
    UBYTE           scan_ssid[NW_SSID_MAX];  /* the network selected      */
    LONG            scan_ssid_len;
    UBYTE           join_bssid[6];          /* before the restart          */
    BOOL            join_down;
    LONG            join_ssid_len;
    LONG            join_wait;              /* seconds left; 0: not waiting */
    BOOL            join_saved;
    BOOL            join_formless;          /* a known network, no form    */
    BOOL            join_hidden, join_open; /* its block's, for a retry    */
    BOOL            pass_saved;             /* "(saved)" in the field      */
    BOOL            wm_seen;                /* WirelessManager ran         */
    UBYTE           conn_ssid[NW_SSID_MAX]; /* the network in use          */
    LONG            conn_len;               /* -1: none                    */

    /* one second, from timer.device */
    struct MsgPort     *tport;
    struct timerequest *treq;
    BOOL                tarmed;
    ULONG               ticks;

    int             result;
    BOOL            done;
} nw;                               /* BSS: 6 KB the file does not carry */

static struct Hook nw_edit_hook, nw_row_hook;

/* The Known networks window: Wireless.prefs as it was read, its blocks in
   the order shown (highest priority first), and the ones removed. */
static struct
{
    char       *text;
    ULONG       len;
    NwNetwork   nets[NW_NETS_MAX];
    LONG        count;
    UBYTE       order[NW_NETS_MAX];
    UBYTE       was[NW_NETS_MAX];      /* the order as opened */
    LONG        shown;
    LONG        sel;
    BOOL        sel_fresh;      /* picked, and nothing edited since    */
    ULONG       click_secs, click_micros;
    struct List list;
    struct Node node[NW_NETS_MAX];
    char        name[NW_NETS_MAX][NW_SSID_MAX + 16];
} kn;

static VOID nw_known_joining(VOID);
static VOID nw_known_fill(VOID);
static VOID nw_rank_scan(VOID);

/* What nw_known_network() answers for a network not in Wireless.prefs. */
static const char nw_unknown[] = "";
#define NW_KNOWN_CHECK  0           /* only whether it is known          */
#define NW_KNOWN_USE    1           /* to the top, in ENV:, and joined   */
#define NW_KNOWN_SAVE   2           /* the same in ENVARC: as well       */
static const char *nw_known_network(const UBYTE *ssid, LONG len, int what);

static BOOL nw_same(const UBYTE *a, LONG al, const UBYTE *b, LONG bl)
{
    LONG i;

    if (al != bl || al < 0) return FALSE;
    for (i = 0; i < al && a[i] == b[i]; i++)
        ;
    return i == al;
}

static ULONG nw_len(const char *s)
{
    ULONG n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static VOID nw_append(char *buf, ULONG cap, const char *s)
{
    ULONG n = nw_len(buf);

    tool_copy_string(buf + n, cap - n, s);
}

static VOID nw_decimal(LONG v, char *out)
{
    char  rev[12];
    int   n = 0;
    ULONG m = v < 0 ? (ULONG)(-v) : (ULONG)v;

    do rev[n++] = (char)('0' + m % 10); while ((m /= 10) != 0);
    if (v < 0) *out++ = '-';
    while (n > 0) *out++ = rev[--n];
    *out = '\0';
}

/* DEVICE as given (argument, then ToolType); else the named interface
   file's; else the first interface file with a Wi-Fi driver (wifipi
   first); else the Pi Wi-Fi's own driver.  FALSE for a named
   interface that names no DEVICE. */
static BOOL nw_pick_device(const char *device, LONG unit, const char *iface)
{
    nw.unit = 0;
    if (device != NULL)
        tool_copy_string(nw.device, sizeof(nw.device), device);
    else if (iface != NULL)
    {
        if (!nw_dev_from_interface(iface, nw.device, sizeof(nw.device), &nw.unit))
            return FALSE;
    }
    else if (!nw_dev_find(nw.device, sizeof(nw.device), &nw.unit))
        tool_copy_string(nw.device, sizeof(nw.device), NW_DEFAULT_DEVICE);
    if (unit >= 0) nw.unit = (ULONG)unit;
    return TRUE;
}

/* --------------------------------------------------------- the Shell -- */

static VOID nw_print_ssid(const UBYTE *ssid, LONG len, ULONG width)
{
    char  text[NW_SSID_MAX + 1];
    ULONG n;

    nw_ssid_text(ssid, (int)len, text);
    tool_printf("%s", (LONG)text);
    for (n = (ULONG)len; n < width; n++) tool_printf(" ");
}

static LONG nw_shell_status(VOID)
{
    LONG  signal = 0, noise = 0, ssid_len = -1, state;
    UBYTE ssid[NW_SSID_MAX], bssid[6];
    char  mac[20];

    state = nw_dev_link(&nw.dev, &signal, &noise);
    tool_printf("Device        %s, unit %lu\n", (LONG)nw.device, (LONG)nw.unit);
    tool_printf("Supplicant    WirelessManager %s\n",
                (LONG)(nw_wm_running(NULL, 0) ? "running" : "not running"));
    switch (state)
    {
        case NW_LINK_OFF:
            tool_printf("Link          off\n");
            return RETURN_WARN;
        case NW_LINK_DOWN:
            tool_printf("Link          not connected\n");
            return RETURN_WARN;
        case NW_LINK_ONLINE:
            tool_printf("Link          online; the driver does not say whether it is associated\n");
            return RETURN_WARN;
        case NW_LINK_ERROR:
            tool_printf("Link          no answer (error %ld, wire error %lu)\n",
                        (LONG)nw.dev.req->ios2_Req.io_Error,
                        nw.dev.req->ios2_WireError);
            return RETURN_ERROR;
        default:
            break;
    }
    tool_printf("Link          connected\n");
    for (ssid_len = 0; ssid_len < 6; ssid_len++) bssid[ssid_len] = 0;
    if (nw_dev_network(&nw.dev, ssid, &ssid_len, bssid))
    {
        /* prism2.device reports the access point and not the name. */
        if (ssid_len >= 0)
        {
            tool_printf("Network       ");
            nw_print_ssid(ssid, ssid_len, 0);
            tool_printf("\n");
        }
        tool_format_mac(bssid, mac, sizeof(mac));
        tool_printf("Access point  %s\n", (LONG)mac);
        if (nw.dev.net_channel > 0)
            tool_printf("Channel       %ld%s\n", nw.dev.net_channel,
                        (LONG)(nw.dev.net_band5 == 1 ? ", 5 GHz"
                               : nw.dev.net_band5 == 0 ? ", 2.4 GHz" : ""));
    }
    tool_printf("Signal        %ld dBm, %ld of 4 bars\n", signal,
                (LONG)nw_bars(signal));
    if (noise != 0) tool_printf("Noise         %ld dBm\n", noise);
    return RETURN_OK;
}

/* Brings the unit up and starts a scan.  Returns NULL or the reason. */
static const char *nw_scan_begin(VOID)
{
    if (SysBase->LibNode.lib_Version < 39)
        return "Scanning needs Kickstart 3.0 or later.";
    if (!nw_dev_bring_up(&nw.dev))
        return "The Wi-Fi unit could not be configured.";
    if (!nw_dev_scan_start(&nw.dev, NULL, 0))
        return "Not enough memory to scan.";
    return NULL;
}

static LONG nw_shell_list(VOID)
{
    const char *why = nw_scan_begin();
    ULONG       sigs, width = 7;
    LONG        n = -1, i;
    BOOL        broke = FALSE;

    if (why != NULL)
    {
        tool_error("%s", (LONG)why);
        return RETURN_ERROR;
    }
    /* A running scan is never aborted: Ctrl-C is noted and waited out. */
    while (nw.dev.scan_busy)
    {
        sigs = Wait((1UL << nw.dev.async->mp_SigBit) | SIGBREAKF_CTRL_C);
        if ((sigs & SIGBREAKF_CTRL_C) && !broke)
        {
            broke = TRUE;
            tool_say("Waiting for the scan to finish.\n");
        }
        if (nw_dev_reply(&nw.dev) == NW_REPLY_SCAN)
            n = nw_dev_scan_collect(&nw.dev, nw.nets, NW_LIST_MAX);
    }
    if (broke) return RETURN_WARN;
    if (n < 0)
    {
        tool_error("the scan failed (error %ld, wire error %lu)",
                   (LONG)nw.dev.scan->ios2_Req.io_Error,
                   nw.dev.scan->ios2_WireError);
        return RETURN_ERROR;
    }

    /* As the window lists them: the network in use, the known ones, the
       rest. */
    {
        LONG  sig, noise, len = -1;
        UBYTE bssid[6];

        if (nw_dev_link(&nw.dev, &sig, &noise) == NW_LINK_UP &&
            nw_dev_network(&nw.dev, nw.conn_ssid, &len, bssid) && len >= 0)
            nw.conn_len = len;
        nw.count = n;
        nw_rank_scan();
    }
    for (i = 0; i < n; i++)
        if ((ULONG)nw.nets[i].ssid_len > width) width = (ULONG)nw.nets[i].ssid_len;
    tool_printf("Network");
    for (i = 7; i < (LONG)width; i++) tool_printf(" ");
    tool_printf("  Signal     Ch  Band  Security\n");
    for (i = 0; i < n; i++)
    {
        const NwScan *e = &nw.nets[i];

        nw_print_ssid(e->ssid, e->ssid_len, width);
        tool_printf("  %4ld dBm  %3ld  %s  %s%s\n", e->signal, (LONG)e->channel,
                    (LONG)(e->band5 ? "5   " : "2.4 "),
                    (LONG)nw_security_label(e->sec),
                    (LONG)(e->rank == NW_RANK_CONNECTED ? "  (connected)"
                           : e->rank != NW_RANK_OTHER ? "  (known)" : ""));
    }
    if (n == 0) tool_printf("No networks in range.\n");
    return RETURN_OK;
}

/* ------------------------------------------------- Wireless.prefs -- */

/* A file buffer, its first len bytes cleared before it is freed: it can
   hold a psk. */
static VOID nw_free_file(char *buf, ULONG len)
{
    volatile char *v = buf;
    ULONG          i;

    if (buf == NULL) return;
    if (len > NW_FILE_MAX) len = NW_FILE_MAX;
    for (i = 0; i < len; i++) v[i] = 0;
    FreeVec(buf);
}

/* The file, or NULL with *missing TRUE when it is not there and FALSE when
   it could not be read: only a missing file may be written as new. */
static char *nw_read(const char *path, ULONG *len, BOOL *missing)
{
    BPTR  fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    char *buf;
    LONG  got;

    *len = 0;
    *missing = FALSE;
    if (fh == (BPTR)0)
    {
        *missing = IoErr() == ERROR_OBJECT_NOT_FOUND;
        return NULL;
    }
    buf = (char *)AllocVec(NW_FILE_MAX, MEMF_ANY);
    got = buf != NULL ? Read(fh, buf, NW_FILE_MAX) : -1;
    Close(fh);
    if (got < 0 || got >= (LONG)NW_FILE_MAX)
    {
        nw_free_file(buf, got < 0 ? NW_FILE_MAX : (ULONG)got);
        return NULL;
    }
    *len = (ULONG)got;
    return buf;
}

/* ENV: is what WirelessManager reads; ENVARC: when ENV: has none. */
static char *nw_read_prefs(ULONG *len, BOOL *missing)
{
    char *t = nw_read(nw_prefs_env, len, missing);

    if (t == NULL && *missing) t = nw_read(nw_prefs_envarc, len, missing);
    return t;
}

/* A new file beside the old one, then renamed over it. */
static BOOL nw_write(const char *path, const char *data, ULONG len)
{
    char tmp[sizeof(nw.to) + 8];
    BPTR fh;
    BOOL ok;

    tool_copy_string(tmp, sizeof(tmp), path);
    nw_append(tmp, sizeof(tmp), ".new");
    fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (fh == (BPTR)0) return FALSE;
    ok = Write(fh, (APTR)data, (LONG)len) == (LONG)len;
    Close(fh);
    if (ok)
    {
        (VOID)DeleteFile((CONST_STRPTR)path);
        ok = Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path) ? TRUE : FALSE;
    }
    if (!ok) (VOID)DeleteFile((CONST_STRPTR)tmp);
    return ok;
}

static VOID nw_make_dir(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);

    if (lock == (BPTR)0) lock = CreateDir((CONST_STRPTR)path);
    if (lock != (BPTR)0) UnLock(lock);
}

/* text's blocks stored in order[0..shown-1], the top one highest, others
   removed (nw_prefs_order_edits()), into ENV:, and ENVARC: first with
   save.  NULL or what went wrong. */
static const char *nw_store_order(const char *text, ULONG tlen,
                                  const NwNetwork *nets, LONG count,
                                  const UBYTE *order, LONG shown, BOOL save)
{
    static NwBlockEdit e[NW_NETS_MAX];
    static NwField     f[NW_NETS_MAX];
    static char        prio[NW_NETS_MAX][12];
    char              *out;
    size_t             len = NW_FILE_MAX;
    const char        *why = NULL;
    int                n;

    n = nw_prefs_order_edits(nets, (int)count, order, (int)shown, e, f, prio);
    out = (char *)AllocVec(NW_FILE_MAX, MEMF_ANY);
    if (out == NULL)
        why = "Not enough memory.";
    else if (!nw_prefs_write(text != NULL ? text : "", tlen, nets, (int)count,
                             e, n, out, NW_FILE_MAX - 1, &len))
    {
        len = NW_FILE_MAX;
        why = "Wireless.prefs would be too large.";
    }
    else
    {
        /* ENVARC: first, as a join writes them. */
        if (save)
        {
            nw_make_dir("ENVARC:Sys");
            if (!nw_write(nw_prefs_envarc, out, (ULONG)len))
                why = "Wireless.prefs could not be written.";
        }
        nw_make_dir("ENV:Sys");
        if (why == NULL && !nw_write(nw_prefs_env, out, (ULONG)len))
            why = "Wireless.prefs could not be written.";
    }
    nw_free_file(out, (ULONG)len);
    return why;
}

/* The joined network into ENVARC: and ENV:, above every other: its block
   edited in place, or a new one.  key NULL is an open network.  Returns
   NULL or what went wrong. */
static const char *nw_save_network(const UBYTE *ssid, LONG ssid_len,
                                   const char *key, BOOL hidden)
{
    static NwNetwork nets[NW_NETS_MAX];
    static char      ssid_value[2 * NW_SSID_MAX + 3];
    static char      priority[12];
    NwField          f[5];
    NwBlockEdit      e;
    char            *old, *out;
    ULONG            old_len;
    size_t           new_len = NW_FILE_MAX;
    LONG             count, i, k, top = 0;
    const char      *why = NULL;
    BOOL             missing;

    /* ENV: is what WirelessManager reads; ENVARC: when ENV: has none.
       SETUP TO: that file alone. */
    if (nw.to[0] != '\0')
        old = nw_read(nw.to, &old_len, &missing);
    else
        old = nw_read_prefs(&old_len, &missing);
    if (old == NULL && !missing)
        return "Wireless.prefs could not be read, so it was not changed.";
    count = nw_prefs_parse(old != NULL ? old : "", old_len, nets, NW_NETS_MAX);
    if (count < 0 || count > NW_NETS_MAX)
    {
        nw_free_file(old, old_len);
        return count < 0 ? "Wireless.prefs has a network={ with no closing }."
                         : "Wireless.prefs has more networks than this edits.";
    }

    e.index = -1;
    for (i = 0; i < count; i++)
    {
        if (nets[i].ssid_len == ssid_len)
        {
            for (k = 0; k < ssid_len && nets[i].ssid[k] == ssid[k]; k++)
                ;
            if (k == ssid_len)
            {
                e.index = (int)i;
                continue;
            }
        }
        if (nets[i].priority >= top) top = nets[i].priority + 1;
    }
    nw_prefs_ssid_value(ssid, (int)ssid_len, ssid_value);
    nw_decimal(top, priority);

    k = 0;
    f[k].key = "ssid";      f[k++].value = ssid_value;
    f[k].key = "psk";       f[k++].value = key;
    f[k].key = "key_mgmt";  f[k++].value = key != NULL ? "WPA-PSK" : "NONE";
    f[k].key = "priority";  f[k++].value = priority;
    f[k].key = "scan_ssid"; f[k++].value = hidden ? "1" : NULL;
    e.remove = 0;
    e.fields = f;
    e.nfields = (int)k;

    /* No larger than the next read accepts. */
    out = (char *)AllocVec(NW_FILE_MAX, MEMF_ANY);
    if (out == NULL)
        why = "Not enough memory.";
    else if (!nw_prefs_write(old != NULL ? old : "", old_len, nets, (int)count,
                             &e, 1, out, NW_FILE_MAX - 1, &new_len))
    {
        new_len = NW_FILE_MAX;
        why = "Wireless.prefs would be too large.";
    }
    else if (nw.to[0] != '\0')
    {
        if (!nw_write(nw.to, out, (ULONG)new_len))
            why = "Wireless.prefs could not be written.";
    }
    else
    {
        nw_make_dir("ENVARC:Sys");
        nw_make_dir("ENV:Sys");
        if (!nw_write(nw_prefs_envarc, out, (ULONG)new_len) ||
            !nw_write(nw_prefs_env, out, (ULONG)new_len))
            why = "Wireless.prefs could not be written.";
    }
    nw_free_file(out, (ULONG)new_len);
    nw_free_file(old, old_len);
    return why;
}

/* ---------------------------------------------------------- gadgets -- */

static VOID nw_set(struct Window *w, int id, ULONG tag, ULONG value)
{
    struct TagItem t[2];

    if (nw.g[id] == NULL) return;
    t[0].ti_Tag = tag;      t[0].ti_Data = value;
    t[1].ti_Tag = TAG_DONE; t[1].ti_Data = 0;
    GT_SetGadgetAttrsA(nw.g[id], w, NULL, t);
}

static VOID nw_requester(const char *body)
{
    struct EasyStruct es;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (STRPTR)"Wi-Fi";
    es.es_TextFormat   = (STRPTR)"%s";
    es.es_GadgetFormat = (STRPTR)"OK";
    (VOID)EasyRequest(nw.main.win, &es, NULL, (ULONG)body);
}

/* The hidden passphrase: the gadget holds '*', nw.pass the text. */
static ULONG nw_edit(register struct Hook *h __asm("a0"),
                     register struct SGWork *sgw __asm("a2"),
                     register ULONG *msg __asm("a1"))
{
    int op;

    (VOID)h;
    if (*msg != SGH_KEY) return *msg == SGH_CLICK;
    if ((sgw->Actions & SGA_USE) == 0) return 1;
    if (nw.pass_saved)
    {
        /* "(saved)" is not text: the first change replaces it. */
        if (sgw->EditOp == EO_NOOP || sgw->EditOp == EO_MOVECURSOR ||
            sgw->EditOp == EO_ENTER)
            return 1;
        nw.pass_saved = FALSE;
        nw.pass_len = 0;
        sgw->NumChars = 0;
        sgw->BufferPos = 0;
        if (sgw->EditOp == EO_INSERTCHAR || sgw->EditOp == EO_REPLACECHAR)
        {
            nw.pass[nw.pass_len++] = (char)sgw->Code;
            sgw->WorkBuffer[0] = nw.pass_shown ? (char)sgw->Code : '*';
            sgw->NumChars = sgw->BufferPos = 1;
        }
        nw.pass[nw.pass_len] = '\0';
        sgw->WorkBuffer[sgw->NumChars] = '\0';
        return 1;
    }
    if (nw.pass_shown)
    {
        tool_copy_string(nw.pass, sizeof(nw.pass), (const char *)sgw->WorkBuffer);
        nw.pass_len = (int)sgw->NumChars;
        return 1;
    }
    switch (sgw->EditOp)
    {
        case EO_INSERTCHAR:  op = NW_EDIT_INSERT; break;
        case EO_REPLACECHAR: op = NW_EDIT_REPLACE; break;
        case EO_NOOP: case EO_DELBACKWARD: case EO_DELFORWARD:
        case EO_MOVECURSOR: case EO_ENTER: case EO_CLEAR:
            op = NW_EDIT_OTHER; break;
        default:
            sgw->Actions = (sgw->Actions & ~SGA_USE) | SGA_BEEP;
            return 1;
    }
    if (!nw_secret_edit(nw.pass, &nw.pass_len, sizeof(nw.pass), op,
                        (int)sgw->BufferPos, (int)sgw->NumChars, (char)sgw->Code))
    {
        sgw->Actions = (sgw->Actions & ~SGA_USE) | SGA_BEEP;
        return 1;
    }
    if (op != NW_EDIT_OTHER) sgw->WorkBuffer[sgw->BufferPos - 1] = '*';
    return 1;
}

/* A list row on GadTools V39: bars, SSID, level, and a padlock when the
   network asks for a key. */
static ULONG nw_draw_row(register struct Hook *h __asm("a0"),
                         register struct Node *node __asm("a2"),
                         register struct LVDrawMsg *m __asm("a1"))
{
    struct RastPort *rp = m->lvdm_RastPort;
    UWORD           *pens = m->lvdm_DrawInfo->dri_Pens;
    const NwScan    *e = &nw.nets[node - nw.row];
    WORD             x = m->lvdm_Bounds.MinX, y = m->lvdm_Bounds.MinY;
    WORD             fh = m->lvdm_Bounds.MaxY - y + 1;
    BOOL             sel = m->lvdm_State == LVR_SELECTED;
    char             text[NW_SSID_MAX + 1];
    const char      *mark = e->rank == NW_RANK_CONNECTED ? " (connected)"
                          : e->rank != NW_RANK_OTHER ? " (known)" : "";
    int              b, bars = nw_bars(e->signal), x0, y0, x1, y1;
    LONG             room;
    ULONG            n;

    (VOID)h;
    if (m->lvdm_MethodID != LV_DRAW) return LVCB_UNKNOWN;
    SetAPen(rp, pens[sel ? FILLPEN : BACKGROUNDPEN]);
    RectFill(rp, x, y, m->lvdm_Bounds.MaxX, m->lvdm_Bounds.MaxY);
    SetAPen(rp, pens[sel ? FILLTEXTPEN : TEXTPEN]);
    SetDrMd(rp, JAM1);

    for (b = 0; b < 4; b++)
    {
        nw_bar_rect(b, nw.lay.bars_w, fh - 2, &x0, &y0, &x1, &y1);
        x0 += x + 2; x1 += x + 2; y0 += y + 1; y1 += y + 1;
        if (b < bars) RectFill(rp, x0, y0, x1, y1);
        else
        {
            Move(rp, x0, y1); Draw(rp, x0, y0); Draw(rp, x1, y0);
            Draw(rp, x1, y1); Draw(rp, x0, y1);
        }
    }
    /* The name shortened, never the marker. */
    nw_ssid_text(e->ssid, e->ssid_len, text);
    room = nw.lay.level_x - nw.lay.bars_w - 12 -
           (LONG)TextLength(rp, (CONST_STRPTR)mark, nw_len(mark));
    for (n = nw_len(text); n > 0 && (LONG)TextLength(rp, (CONST_STRPTR)text, n) > room;
         n--)
        ;
    Move(rp, x + nw.lay.bars_w + 8, y + rp->TxBaseline);
    Text(rp, (CONST_STRPTR)text, n);
    Text(rp, (CONST_STRPTR)mark, nw_len(mark));
    nw_decimal(e->signal, text);
    nw_append(text, sizeof(text), " dBm");
    Move(rp, x + nw.lay.level_x, y + rp->TxBaseline);
    Text(rp, (CONST_STRPTR)text, nw_len(text));
    if (e->sec != 0)
    {
        /* A padlock: the shackle's outline over a solid body. */
        x0 = x + nw.lay.lock_x;
        y0 = y + 1;
        x1 = x0 + fh - 3;
        y1 = y0 + (fh - 2) / 2;
        RectFill(rp, x0, y1, x1, y + fh - 2);
        Move(rp, x0 + 2, y1); Draw(rp, x0 + 2, y0);
        Draw(rp, x1 - 2, y0); Draw(rp, x1 - 2, y1);
    }
    return LVCB_OK;
}

/* The scan list's order: the network in use, then the known ones by
   priority, then the rest as scanned. */
static NwNetwork nw_kn_nets[NW_NETS_MAX];   /* Wireless.prefs, read here   */

static VOID nw_rank_scan(VOID)
{
    UBYTE  order[NW_NETS_MAX];
    char  *text;
    ULONG  len;
    BOOL   missing;
    LONG   count = 0, i, r;

    text = nw_read_prefs(&len, &missing);
    if (text != NULL)
    {
        count = nw_prefs_parse(text, len, nw_kn_nets, NW_NETS_MAX);
        if (count < 0) count = 0;
        if (count > NW_NETS_MAX) count = NW_NETS_MAX;
        nw_prefs_order(nw_kn_nets, (int)count, order);
    }
    for (i = 0; i < nw.count; i++)
    {
        NwScan *e = &nw.nets[i];

        r = count > 0 ? nw_prefs_rank(nw_kn_nets, order, (int)count, e->ssid,
                                      e->ssid_len) : -1;
        e->rank = nw_same(e->ssid, e->ssid_len, nw.conn_ssid, nw.conn_len)
                  ? NW_RANK_CONNECTED : r >= 0 ? (int)r + 1 : NW_RANK_OTHER;
    }
    nw_scan_rank_sort(nw.nets, (int)nw.count);
    nw_free_file(text, len);
}

/* The list rows from nw.nets: their text for GadTools V37, drawn on V39. */
static VOID nw_fill_list(VOID)
{
    LONG i;

    nw_set(nw.main.win, NW_M_LIST, GTLV_Labels, ~0UL);
    nw.rows.lh_Head = (struct Node *)&nw.rows.lh_Tail;
    nw.rows.lh_Tail = NULL;
    nw.rows.lh_TailPred = (struct Node *)&nw.rows.lh_Head;
    for (i = 0; i < nw.count; i++)
    {
        const NwScan *e = &nw.nets[i];
        char         *t = nw.row_text[i];

        nw_ssid_text(e->ssid, e->ssid_len, t);
        nw_append(t, NW_ROW_LEN, e->rank == NW_RANK_CONNECTED ? " (connected)"
                               : e->rank != NW_RANK_OTHER ? " (known)" : "");
        nw_append(t, NW_ROW_LEN, "  ");
        nw_decimal(e->signal, t + nw_len(t));
        nw_append(t, NW_ROW_LEN, " dBm  ");
        nw_append(t, NW_ROW_LEN, nw_security_label(e->sec));
        nw.row[i].ln_Name = t;
        AddTail(&nw.rows, &nw.row[i]);
    }
    /* The selection follows its network into the new order. */
    nw.selected = -1;
    for (i = 0; i < nw.count && nw.selected < 0; i++)
        if (nw_same(nw.nets[i].ssid, nw.nets[i].ssid_len, nw.sel_ssid, nw.sel_len))
            nw.selected = i;
    {
        struct TagItem t[4];

        t[0].ti_Tag = GTLV_Labels;   t[0].ti_Data = (ULONG)&nw.rows;
        t[1].ti_Tag = GTLV_Selected; t[1].ti_Data = nw.selected >= 0 ? (ULONG)nw.selected : ~0UL;
        t[2].ti_Tag = TAG_DONE;
        if (nw.selected >= 0 && GadToolsBase->lib_Version >= 39)
        {
            t[2].ti_Tag = GTLV_MakeVisible; t[2].ti_Data = (ULONG)nw.selected;
            t[3].ti_Tag = TAG_DONE;
        }
        if (nw.g[NW_M_LIST] != NULL)
            GT_SetGadgetAttrsA(nw.g[NW_M_LIST], nw.main.win, NULL, t);
    }
}

/* The Network and Signal lines, from the unit. */
static VOID nw_refresh_status(VOID)
{
    static const char *const words[] =
        { "No Wi-Fi unit", "Wi-Fi off", "Not connected", "Connected", "Online", "No answer" };
    LONG  signal = 0, noise = 0, len = -1, link, i;
    UBYTE ssid[NW_SSID_MAX], bssid[6];
    char  bars[2];
    LONG  was = nw.conn_len;

    nw.sig_text[0] = '\0';
    link = nw.dev_open ? nw_dev_link(&nw.dev, &signal, &noise) : NW_LINK_NODEV;
    /* As NetWireless: WirelessManager gone after it ran is the Wi-Fi off,
       whatever the radio still says. */
    if (!nw.setup && (link == NW_LINK_UP || link == NW_LINK_DOWN || link == NW_LINK_ONLINE))
    {
        if (nw_wm_running(NULL, 0)) nw.wm_seen = TRUE;
        else if (nw.wm_seen || nw_wm_marked_off()) link = NW_LINK_OFF;
    }
    if (link != NW_LINK_UP) nw.shown_bars = 0;
    tool_copy_string(nw.net_text, sizeof(nw.net_text), words[link]);
    if (link == NW_LINK_UP)
    {
        if (nw_dev_network(&nw.dev, ssid, &len, bssid) && len >= 0)
            nw_ssid_text(ssid, (int)len, nw.net_text);
        else
            len = -1;
        nw_decimal(signal, nw.sig_text);
        nw_append(nw.sig_text, sizeof(nw.sig_text), " dBm, ");
        /* With the commodity's hysteresis, so the two agree. */
        nw.shown_bars = nw_bars_held(signal, nw.shown_bars);
        bars[0] = (char)('0' + nw.shown_bars);
        bars[1] = '\0';
        nw_append(nw.sig_text, sizeof(nw.sig_text), bars);
        nw_append(nw.sig_text, sizeof(nw.sig_text), " of 4 bars");
        if (len >= 0 && nw.dev.net_channel > 0)
        {
            nw_append(nw.sig_text, sizeof(nw.sig_text), ", channel ");
            nw_decimal(nw.dev.net_channel, nw.sig_text + nw_len(nw.sig_text));
            if (nw.dev.net_band5 >= 0)
                nw_append(nw.sig_text, sizeof(nw.sig_text),
                          nw.dev.net_band5 ? ", 5 GHz" : ", 2.4 GHz");
        }
    }
    nw_set(nw.main.win, NW_M_NETWORK, GTTX_Text, (ULONG)nw.net_text);
    nw_set(nw.main.win, NW_M_SIGNAL, GTTX_Text, (ULONG)nw.sig_text);
    nw_set(nw.main.win, NW_M_DISCONNECT, GA_Disabled,
           nw.setup || !nw_wm_running(NULL, 0));
    /* The network in use changed: both lists say so. */
    if (link != NW_LINK_UP) len = -1;
    if (!nw_same(ssid, len, nw.conn_ssid, nw.conn_len) && (len >= 0 || was >= 0))
    {
        for (i = 0; i < len; i++) nw.conn_ssid[i] = ssid[i];
        nw.conn_len = len;
        nw_rank_scan();
        nw_fill_list();
        nw_known_fill();
    }
}

static VOID nw_scan(VOID)
{
    const char *why;

    if (!nw.dev_open || nw.dev.scan_busy) return;
    why = nw_scan_begin();
    if (why != NULL)
    {
        nw_requester(why);
        return;
    }
    nw_set(nw.main.win, NW_M_SCAN, GA_Disabled, TRUE);
    SetWindowTitles(nw.main.win, (STRPTR)"Wi-Fi: scanning", (STRPTR)~0);
}

static VOID nw_scan_done(VOID)
{
    LONG n = nw_dev_scan_collect(&nw.dev, nw.nets, NW_LIST_MAX);

    nw.count = n < 0 ? 0 : n;
    nw_rank_scan();
    nw_fill_list();
    nw_set(nw.main.win, NW_M_SCAN, GA_Disabled, FALSE);
    SetWindowTitles(nw.main.win, (STRPTR)(nw.setup ? "Wi-Fi Setup" : "Wi-Fi"),
                    (STRPTR)~0);
    if (n < 0) nw_requester("The scan failed.");
}

/* --------------------------------------------------- window building -- */

static struct TextAttr nw_topaz8 =
{
    (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT
};

static int nw_measure(void *ctx, const char *text, int len)
{
    return (int)TextLength((struct RastPort *)ctx, (CONST_STRPTR)text, (ULONG)len);
}

static BOOL nw_layout_in(struct TextAttr *ta)
{
    struct Screen   *s = nw.screen;
    struct TextFont *font = OpenFont(ta);
    struct RastPort  rp;
    NwFont           metrics;

    if (font == NULL) return FALSE;
    InitRastPort(&rp);
    SetFont(&rp, font);
    metrics.height = font->tf_YSize;
    metrics.measure = nw_measure;
    metrics.ctx = &rp;
    if (!nw_layout(&metrics, s->Width - s->WBorLeft - s->WBorRight,
                   s->Height - s->WBorTop - s->Font->ta_YSize - 1 - s->WBorBottom,
                   &nw.lay))
    {
        CloseFont(font);
        return FALSE;
    }
    nw.font = font;
    nw.ta = ta;
    return TRUE;
}

static const ULONG nw_place[] =
{
    [NW_PLACE_NONE] = 0, [NW_PLACE_LEFT] = PLACETEXT_LEFT,
    [NW_PLACE_RIGHT] = PLACETEXT_RIGHT, [NW_PLACE_ABOVE] = PLACETEXT_ABOVE,
    [NW_PLACE_IN] = PLACETEXT_IN
};

static struct Gadget *nw_gadget(ULONG kind, struct Gadget *prev, int id,
                                struct TagItem *tags)
{
    struct NewGadget ng;
    const NwBox     *b = &nw.lay.box[id];

    if (prev == NULL) return NULL;
    ng.ng_LeftEdge = (WORD)b->x;  ng.ng_TopEdge = (WORD)b->y;
    ng.ng_Width = (WORD)b->w;     ng.ng_Height = (WORD)b->h;
    ng.ng_GadgetText = (STRPTR)nw_items[id].label;
    ng.ng_TextAttr = nw.ta;
    ng.ng_GadgetID = (UWORD)id;
    ng.ng_Flags = nw_place[nw_items[id].place];
    ng.ng_VisualInfo = nw.visual;
    ng.ng_UserData = NULL;
    nw.g[id] = CreateGadgetA(kind, prev, &ng, tags);
    return nw.g[id];
}

#define NW_IDCMP (IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MENUPICK | \
                  BUTTONIDCMP | LISTVIEWIDCMP | STRINGIDCMP | CHECKBOXIDCMP | \
                  CYCLEIDCMP)

/* Centred unless the icon's snapshot says where. */
static struct Window *nw_open(int which, struct Gadget *glist, const char *title,
                              ULONG idcmp, WORD left, WORD top)
{
    static WORD    zoom[4];
    struct TagItem t[13];
    WORD w = (WORD)(nw.lay.inner_w[which] + nw.screen->WBorLeft + nw.screen->WBorRight);
    WORD h = (WORD)(nw.lay.inner_h[which] + nw.screen->WBorTop +
                    nw.screen->Font->ta_YSize + 1 + nw.screen->WBorBottom);
    int  k = 0;

    if (left < 0 || left + w > nw.screen->Width) left = (WORD)((nw.screen->Width - w) / 2);
    if (top < 0 || top + h > nw.screen->Height) top = (WORD)((nw.screen->Height - h) / 2);
#define T(a, b) do { t[k].ti_Tag = (a); t[k].ti_Data = (ULONG)(b); k++; } while (0)
    T(WA_Left, left);
    T(WA_Top, top);
    T(WA_InnerWidth, nw.lay.inner_w[which]);
    T(WA_InnerHeight, nw.lay.inner_h[which]);
    T(WA_Title, title);
    T(WA_IDCMP, idcmp);
    T(WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                WFLG_ACTIVATE | WFLG_SMART_REFRESH | WFLG_GIMMEZEROZERO);
    T(WA_Gadgets, glist);
    T(WA_PubScreen, nw.screen);
    T(WA_NewLookMenus, TRUE);
    T(WA_AutoAdjust, TRUE);
    if (which == NW_WIN_MAIN)
    {
        /* The zoom gadget folds the window to its title bar, where it
           stands from V39 (~0), where it opened before.  Intuition does
           not fold below the borders and one inner line. */
        zoom[0] = IntuitionBase->LibNode.lib_Version >= 39 ? (WORD)~0 : left;
        zoom[1] = IntuitionBase->LibNode.lib_Version >= 39 ? (WORD)~0 : top;
        zoom[2] = w;
        zoom[3] = (WORD)(nw.screen->WBorTop + nw.screen->Font->ta_YSize + 1 +
                         nw.screen->WBorBottom + 1);
        T(WA_Zoom, zoom);
    }
    T(TAG_DONE, 0);
#undef T
    return OpenWindowTagList(NULL, t);
}

static BOOL nw_open_main(VOID)
{
    struct Gadget *g;
    struct TagItem t[4];

    g = CreateContext(&nw.main.glist);
    t[0].ti_Tag = GTTX_Border; t[0].ti_Data = TRUE;
    t[1].ti_Tag = TAG_DONE;
    g = nw_gadget(TEXT_KIND, g, NW_M_NETWORK, t);
    g = nw_gadget(TEXT_KIND, g, NW_M_SIGNAL, t);
    nw.rows.lh_Head = (struct Node *)&nw.rows.lh_Tail;
    nw.rows.lh_TailPred = (struct Node *)&nw.rows.lh_Head;
    t[0].ti_Tag = GTLV_Labels;       t[0].ti_Data = (ULONG)&nw.rows;
    t[1].ti_Tag = GTLV_ShowSelected; t[1].ti_Data = 0;
    t[2].ti_Tag = TAG_DONE;
    if (GadToolsBase->lib_Version >= 39)
    {
        nw_row_hook.h_Entry = (ULONG (*)())(APTR)nw_draw_row;
        t[2].ti_Tag = GTLV_CallBack; t[2].ti_Data = (ULONG)&nw_row_hook;
        t[3].ti_Tag = TAG_DONE;
    }
    g = nw_gadget(LISTVIEW_KIND, g, NW_M_LIST, t);
    g = nw_gadget(BUTTON_KIND, g, NW_M_SCAN, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_M_JOIN, NULL);
    t[0].ti_Tag = GA_Disabled; t[0].ti_Data = nw.setup;
    t[1].ti_Tag = TAG_DONE;
    g = nw_gadget(BUTTON_KIND, g, NW_M_KNOWN, t);
    t[0].ti_Data = TRUE;
    g = nw_gadget(BUTTON_KIND, g, NW_M_DISCONNECT, t);
    if (g == NULL) return FALSE;

    nw.main.win = nw_open(NW_WIN_MAIN, nw.main.glist,
                          nw.setup ? "Wi-Fi Setup" : "Wi-Fi",
                          NW_IDCMP, nw.want_left, nw.want_top);
    if (nw.main.win == NULL) return FALSE;
    /* Without a sizing gadget the limits are the opening size until set,
       and the zoom gadget could not fold the window to its title bar. */
    (VOID)WindowLimits(nw.main.win, nw.main.win->Width,
                       (WORD)(nw.screen->WBorTop + nw.screen->Font->ta_YSize + 1 +
                              nw.screen->WBorBottom + 1),
                       nw.main.win->Width, nw.main.win->Height);
    GT_RefreshWindow(nw.main.win, NULL);
    if (!nw.setup && (nw.menu = CreateMenusA(nw_menu, NULL)) != NULL)
    {
        struct TagItem lt[2];

        lt[0].ti_Tag = GTMN_NewLookMenus; lt[0].ti_Data = TRUE;
        lt[1].ti_Tag = TAG_DONE;
        if (!LayoutMenusA(nw.menu, nw.visual, lt) ||
            !SetMenuStrip(nw.main.win, nw.menu))
        {
            FreeMenus(nw.menu);
            nw.menu = NULL;
        }
    }
    return TRUE;
}

/* The passphrase gone from this program's memory and from the gadget. */
static VOID nw_wipe_pass(VOID)
{
    int i;

    for (i = 0; i < (int)sizeof(nw.pass); i++) nw.pass[i] = '\0';
    nw.pass_len = 0;
    nw.pass_saved = FALSE;
    nw_set(nw.join.win, NW_J_PASS, GTST_String, (ULONG)"");
}

static VOID nw_close_join(VOID)
{
    int i;

    nw_wipe_pass();
    if (nw.join.win != NULL) CloseWindow(nw.join.win);
    if (nw.join.glist != NULL) FreeGadgets(nw.join.glist);
    nw.join.win = NULL;
    nw.join.glist = NULL;
    for (i = NW_J_SSID; i <= NW_J_CANCEL; i++) nw.g[i] = NULL;
    if (nw.join_wait != 0 && !nw.join_formless)
    {
        /* The wait ends with the form: no "(connecting...)" left. */
        nw.join_wait = 0;
        nw_known_joining();
        nw_known_fill();
    }
}

static VOID nw_join_status(const char *s)
{
    tool_copy_string(nw.status, sizeof(nw.status), s);
    nw_set(nw.join.win, NW_J_STATUS, GTTX_Text, (ULONG)nw.status);
}

static const char nw_saved_text[] = "(saved)";
static const char nw_saved_note[] = "The saved key is used unless a new one is typed.";

/* The join form for ssid (len < 0: empty, for a hidden network).  A known
   network's field says "(saved)": its stored key is used unless a new
   passphrase is typed. */
static VOID nw_open_form(const UBYTE *ssid, LONG len, BOOL open, BOOL hidden)
{
    static char    ssid_text[NW_SSID_MAX + 1];
    struct Gadget *g;
    struct TagItem t[5];

    if (nw.join.win != NULL)
    {
        WindowToFront(nw.join.win);
        ActivateWindow(nw.join.win);
        return;
    }
    ssid_text[0] = '\0';
    nw.scan_ssid_len = 0;
    if (len >= 0)
    {
        nw_ssid_text(ssid, (int)len, ssid_text);
        CopyMem((APTR)ssid, nw.scan_ssid, (ULONG)len);
        nw.scan_ssid_len = len;
    }
    nw.pass_len = 0;
    nw.pass[0] = '\0';
    nw.pass_shown = FALSE;
    nw.open_network = open;
    nw.pass_saved = !open && len > 0 &&
                    nw_known_network(ssid, len, NW_KNOWN_CHECK) == NULL;
    tool_copy_string(nw.status, sizeof(nw.status),
                     nw.pass_saved ? nw_saved_note : "");

    g = CreateContext(&nw.join.glist);
    t[0].ti_Tag = GTST_String;   t[0].ti_Data = (ULONG)ssid_text;
    t[1].ti_Tag = GTST_MaxChars; t[1].ti_Data = NW_SSID_MAX;
    t[2].ti_Tag = TAG_DONE;
    g = nw_gadget(STRING_KIND, g, NW_J_SSID, t);
    t[0].ti_Tag = GTCY_Labels;   t[0].ti_Data = (ULONG)nw_security_choices;
    t[1].ti_Tag = GTCY_Active;   t[1].ti_Data = nw.open_network;
    g = nw_gadget(CYCLE_KIND, g, NW_J_SECURITY, t);
    nw_edit_hook.h_Entry = (ULONG (*)())(APTR)nw_edit;
    t[0].ti_Tag = GTST_MaxChars; t[0].ti_Data = NW_PASS_MAX;
    t[1].ti_Tag = GTST_EditHook; t[1].ti_Data = (ULONG)&nw_edit_hook;
    t[2].ti_Tag = GA_Disabled;   t[2].ti_Data = nw.open_network;
    t[3].ti_Tag = GTST_String;   t[3].ti_Data = (ULONG)(nw.pass_saved ? nw_saved_text : "");
    t[4].ti_Tag = TAG_DONE;
    g = nw_gadget(STRING_KIND, g, NW_J_PASS, t);
    g = nw_gadget(CHECKBOX_KIND, g, NW_J_SHOW, NULL);
    t[0].ti_Tag = GTCB_Checked;  t[0].ti_Data = hidden;
    t[1].ti_Tag = TAG_DONE;
    g = nw_gadget(CHECKBOX_KIND, g, NW_J_HIDDEN, t);
    t[0].ti_Tag = GTTX_Border;   t[0].ti_Data = TRUE;
    t[1].ti_Tag = GTTX_Text;     t[1].ti_Data = (ULONG)nw.status;
    t[2].ti_Tag = TAG_DONE;
    g = nw_gadget(TEXT_KIND, g, NW_J_STATUS, t);
    g = nw_gadget(BUTTON_KIND, g, NW_J_JOIN, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_J_CANCEL, NULL);
    if (g == NULL ||
        (nw.join.win = nw_open(NW_WIN_JOIN, nw.join.glist, "Join a Wi-Fi network",
                               NW_IDCMP & ~IDCMP_MENUPICK, -1, -1)) == NULL)
    {
        nw_close_join();
        return;
    }
    GT_RefreshWindow(nw.join.win, NULL);
    (VOID)ActivateGadget(nw.g[len >= 0 && !nw.open_network ? NW_J_PASS : NW_J_SSID],
                         nw.join.win, NULL);
}

/* The form for the network selected, or empty for a hidden one. */
static VOID nw_open_join(VOID)
{
    const NwScan *e = nw.selected >= 0 ? &nw.nets[nw.selected] : NULL;

    if (e != NULL && !nw_security_joinable(e->sec))
    {
        nw_requester("WirelessManager cannot join a network with this security.");
        return;
    }
    nw_open_form(e != NULL ? e->ssid : NULL, e != NULL ? e->ssid_len : -1,
                 e != NULL && e->sec == 0, e == NULL);
}

/* The form frozen while the key is derived, Cancel left alone; the busy
   pointer on Intuition V39. */
static VOID nw_join_busy(BOOL busy)
{
    int i;

    for (i = NW_J_SSID; i <= NW_J_JOIN; i++)
        if (i != NW_J_STATUS) nw_set(nw.join.win, i, GA_Disabled, busy);
    if (!busy) nw_set(nw.join.win, NW_J_PASS, GA_Disabled, nw.open_network);
    if (IntuitionBase->LibNode.lib_Version >= 39)
        SetWindowPointer(nw.join.win, WA_BusyPointer, busy, TAG_DONE);
}

/* Between key steps: the percentage shown, Cancel and close honoured. */
static int nw_key_progress(void *ctx, unsigned long done)
{
    struct IntuiMessage *m;
    char  text[40];
    BOOL  cancel = FALSE;

    (VOID)ctx;
    tool_copy_string(text, sizeof(text), "Computing the key: ");
    nw_decimal((LONG)(done * 100UL / NW_KEY_STEPS), text + nw_len(text));
    nw_append(text, sizeof(text), "%");
    nw_join_status(text);
    while ((m = GT_GetIMsg(nw.join.win->UserPort)) != NULL)
    {
        ULONG cls = m->Class;
        BOOL  stop = cls == IDCMP_CLOSEWINDOW ||
                     (cls == IDCMP_GADGETUP &&
                      ((struct Gadget *)m->IAddress)->GadgetID == NW_J_CANCEL);

        GT_ReplyIMsg(m);
        if (stop) cancel = TRUE;
        if (cls == IDCMP_REFRESHWINDOW)
        {
            GT_BeginRefresh(nw.join.win);
            GT_EndRefresh(nw.join.win, TRUE);
        }
    }
    return !cancel;
}

/* The first answer stands: a close queued behind a SETUP that wrote the
   file must not turn 0 into 5. */
static VOID nw_finish(int rc)
{
    if (nw.done) return;
    nw.result = rc;
    nw.done = TRUE;
}

/* The form's SSID text as bytes in out: a scanned name shown unedited is
   its own bytes, which the gadget showed with '?' for control characters.
   Returns the length, 0 when there is none to join. */
static LONG nw_form_ssid(const char *ssid, UBYTE *out)
{
    char hex[NW_SSID_MAX + 1];
    LONG len = (LONG)nw_len(ssid), i;

    if (len < 1 || len > NW_SSID_MAX) return 0;
    nw_ssid_text(nw.scan_ssid, (int)nw.scan_ssid_len, hex);
    for (i = 0; hex[i] != '\0' && hex[i] == ssid[i]; i++)
        ;
    if (nw.scan_ssid_len > 0 && hex[i] == ssid[i])
    {
        len = nw.scan_ssid_len;
        for (i = 0; i < len; i++) out[i] = nw.scan_ssid[i];
    }
    else
        for (i = 0; i < len; i++) out[i] = (UBYTE)ssid[i];
    return len;
}

/* The SSID and passphrase checked, the key derived and the network saved.
   Returns NULL or what went wrong. */
static const char *nw_join_save(const char *ssid, const char *pass, int pass_len,
                                BOOL open, BOOL hidden, NwKeyProgress progress)
{
    const char *why = NULL;
    char        hex[2 * NW_KEY_LEN + 1];
    UBYTE       key[NW_KEY_LEN];
    LONG        len = nw_form_ssid(ssid, nw.join_ssid), kind, i;

    if (len == 0) return "Type the name of the network.";
    nw.join_ssid_len = len;
    if (!open)
    {
        kind = nw_key_kind(pass, (size_t)pass_len);
        if (kind == NW_PASS_BAD) return "A passphrase has 8 to 63 characters.";
        if (kind == NW_PASS_HEX)
            tool_copy_string(hex, sizeof(hex), pass);
        else
        {
            i = nw_key_derive(pass, (size_t)pass_len, nw.join_ssid, (size_t)len,
                              key, progress, NULL);
            if (i) nw_key_hex(key, hex);
            for (kind = 0; kind < NW_KEY_LEN; kind++) key[kind] = 0;
            if (!i) return "Cancelled.";
        }
    }
    why = nw_save_network(nw.join_ssid, len, open ? NULL : hex, hidden);
    for (i = 0; i < (LONG)sizeof(hex); i++) hex[i] = '\0';
    return why;
}

/* WirelessManager stopped and started on the new file.  NULL or why not. */
static const char *nw_restart_wm(VOID)
{
    LONG  len;
    UBYTE ssid[NW_SSID_MAX];
    int   i;

    /* The access point before, for a driver that does not name networks:
       a join is counted once the link went down or the access point
       changed, not because the card is still on the old one. */
    for (i = 0; i < 6; i++) nw.join_bssid[i] = 0;
    nw.join_down = FALSE;
    if (nw.dev_open) (VOID)nw_dev_network(&nw.dev, ssid, &len, nw.join_bssid);
    if (!nw_wm_stop()) return "WirelessManager did not stop.";
    if (!nw_wm_start(nw.wm, nw.device, nw.unit))
        return "WirelessManager could not be started.";
    return NULL;
}

/* WirelessManager restarted for ssid, and the association waited for. */
static const char *nw_start_join(const UBYTE *ssid, LONG len)
{
    const char *why;
    LONG        i;

    for (i = 0; i < len; i++) nw.join_ssid[i] = ssid[i];
    nw.join_ssid_len = len;
    why = nw_restart_wm();
    if (why == NULL)
    {
        nw.join_wait = NW_JOIN_WAIT;
        nw_known_joining();
    }
    return why;
}

/* ssid's block in Wireless.prefs, one WirelessManager can use (a key, or
   open, not disabled): nw_unknown when there is none (or in SETUP).  With
   NW_KNOWN_USE or NW_KNOWN_SAVE it is moved to the top and joined with the
   key it has.  NULL or what went wrong. */
static const char *nw_known_network(const UBYTE *ssid, LONG len, int what)
{
    UBYTE       order[NW_NETS_MAX];
    char       *text;
    ULONG       tlen;
    BOOL        missing;
    LONG        count, i, at = -1;
    const char *why = NULL;

    if (nw.setup || len <= 0) return nw_unknown;
    text = nw_read_prefs(&tlen, &missing);
    if (text == NULL) return nw_unknown;
    count = nw_prefs_parse(text, tlen, nw_kn_nets, NW_NETS_MAX);
    for (i = 0; i < count && i < NW_NETS_MAX && at < 0; i++)
        if (!nw_kn_nets[i].disabled &&
            (nw_kn_nets[i].has_psk || nw_kn_nets[i].open || nw_kn_nets[i].wep) &&
            nw_same(nw_kn_nets[i].ssid, nw_kn_nets[i].ssid_len, ssid, len))
            at = i;
    if (at < 0 || count > NW_NETS_MAX)
    {
        nw_free_file(text, tlen);
        return nw_unknown;
    }
    nw.join_hidden = nw_kn_nets[at].scan_ssid != 0;
    nw.join_open = nw_kn_nets[at].open != 0;
    if (what != NW_KNOWN_CHECK)
    {
        nw_prefs_order(nw_kn_nets, (int)count, order);
        for (i = 0; order[i] != at; i++)
            ;
        for (; i > 0; i--) order[i] = order[i - 1];
        order[0] = (UBYTE)at;
        why = nw_store_order(text, tlen, nw_kn_nets, count, order, count,
                             what == NW_KNOWN_SAVE);
    }
    nw_free_file(text, tlen);
    if (why == NULL && what != NW_KNOWN_CHECK) why = nw_start_join(ssid, len);
    return why;
}

/* Associated with the SSID of the join? */
static BOOL nw_joined(VOID)
{
    LONG  signal, noise, len = -1, i, link, op = -1;
    UBYTE ssid[NW_SSID_MAX], bssid[6];

    if (!nw.dev_open) return FALSE;
    link = nw_dev_link(&nw.dev, &signal, &noise);
    /* prism2.device 2.9's LINKUP is associated and keyed: a wrong key
       associates and never comes up. */
    if (!nw.dev.signal_says) op = nw_dev_operational(&nw.dev);
    if (link == NW_LINK_DOWN || link == NW_LINK_OFF || op == 0)
    {
        nw.join_down = TRUE;
        return FALSE;
    }
    if (op == 1) link = NW_LINK_UP;
    if (link != NW_LINK_UP && link != NW_LINK_ONLINE) return FALSE;
    /* Kickstart 2.04 cannot ask which network. */
    if (SysBase->LibNode.lib_Version < 39) return link == NW_LINK_UP;
    for (i = 0; i < 6; i++) bssid[i] = 0;
    if (!nw_dev_network(&nw.dev, ssid, &len, bssid)) return FALSE;
    if (len < 0)
    {
        /* No network name (prism2.device): joined once the link went down
           on the restart, or the access point changed.  A driver that does
           not report association at all counts only a new access point. */
        for (i = 0; i < 6 && bssid[i] == nw.join_bssid[i]; i++)
            ;
        return link == NW_LINK_UP ? (nw.join_down || i < 6) : i < 6;
    }
    if (link != NW_LINK_UP || len != nw.join_ssid_len) return FALSE;
    for (i = 0; i < len && ssid[i] == nw.join_ssid[i]; i++)
        ;
    return i == len;
}

/* JOIN from a Shell: saved, WirelessManager restarted, 30 s to associate. */
static LONG nw_shell_join(const char *ssid, const char *pass, BOOL hidden)
{
    const char *why = nw_unknown;
    LONG        t;

    /* Without PASSPHRASE: a known network with the key it has, else open. */
    if (pass == NULL)
        why = nw_known_network((const UBYTE *)ssid, (LONG)nw_len(ssid),
                               NW_KNOWN_SAVE);
    if (why == nw_unknown)
    {
        why = nw_join_save(ssid, pass != NULL ? pass : "",
                           pass != NULL ? (int)nw_len(pass) : 0,
                           pass == NULL, hidden, NULL);
        if (why == NULL) why = nw_restart_wm();
    }
    else if (why == NULL)
        tool_printf("Joining the known network.\n");
    if (why != NULL)
    {
        tool_error("%s", (LONG)why);
        return RETURN_ERROR;
    }
    for (t = 0; t < NW_JOIN_WAIT; t++)
    {
        if (nw_joined())
        {
            tool_printf("Connected.\n");
            return RETURN_OK;
        }
        if (tool_delay_ticks(50)) break;
    }
    tool_printf("Not connected after %ld seconds.\n", t);
    return RETURN_WARN;
}

/* Join: the form checked, the key derived, Wireless.prefs written,
   WirelessManager restarted, and the association waited for. */
static VOID nw_join(VOID)
{
    struct StringInfo *si = (struct StringInfo *)nw.g[NW_J_SSID]->SpecialInfo;
    const char *why;
    UBYTE       ssid[NW_SSID_MAX];
    LONG        len;

    if (nw.join_wait != 0) return;      /* one join at a time */
    if (!nw.open_network && nw.pass_len == 0 &&
        (len = nw_form_ssid((const char *)si->Buffer, ssid)) > 0 &&
        (why = nw_known_network(ssid, len, NW_KNOWN_SAVE)) != nw_unknown)
    {
        nw_join_status(why != NULL ? why : "Waiting for the connection.");
        if (why == NULL)
        {
            nw_rank_scan();
            nw_fill_list();
        }
        return;
    }
    nw_join_busy(TRUE);
    why = nw_join_save((const char *)si->Buffer, nw.pass, nw.pass_len,
                       nw.open_network,
                       (nw.g[NW_J_HIDDEN]->Flags & GFLG_SELECTED) != 0,
                       nw_key_progress);
    nw_join_busy(FALSE);
    if (why == NULL) nw_wipe_pass();
    if (why != NULL)
    {
        nw_join_status(why);
        return;
    }
    nw.join_saved = TRUE;
    if (nw.setup)
    {
        nw_finish(NW_SETUP_WRITTEN);
        return;
    }
    nw_join_status("Restarting WirelessManager.");
    why = nw_restart_wm();
    if (why != NULL)
        nw_join_status(why);
    else
    {
        nw.join_wait = NW_JOIN_WAIT;
        nw_known_joining();
        nw_join_status("Waiting for the connection.");
    }
}

/* A join without the form: the main window's two lines say how it goes. */
static VOID nw_show_connecting(const char *text)
{
    char name[NW_SSID_MAX + 1];

    nw_ssid_text(nw.join_ssid, (int)nw.join_ssid_len, name);
    tool_copy_string(nw.net_text, sizeof(nw.net_text), "Connecting to ");
    nw_append(nw.net_text, sizeof(nw.net_text), name);
    tool_copy_string(nw.sig_text, sizeof(nw.sig_text), text);
    nw_set(nw.main.win, NW_M_NETWORK, GTTX_Text, (ULONG)nw.net_text);
    nw_set(nw.main.win, NW_M_SIGNAL, GTTX_Text, (ULONG)nw.sig_text);
}

/* A known network's join begun without the form: the lists reordered,
   and the progress in the main window. */
static VOID nw_connecting(VOID)
{
    nw.join_formless = TRUE;
    nw_rank_scan();
    nw_fill_list();
    nw_known_fill();
    nw_show_connecting("Waiting for the connection.");
}

/* Join in the main window: a known network at once, with its saved key;
   any other through the form. */
static VOID nw_join_pressed(VOID)
{
    const NwScan *e = nw.selected >= 0 ? &nw.nets[nw.selected] : NULL;
    const char   *why;

    /* One join at a time. */
    if (nw.join_wait != 0 && nw.join.win == NULL) return;
    if (e != NULL && nw.join.win == NULL && nw.join_wait == 0)
    {
        why = nw_known_network(e->ssid, e->ssid_len, NW_KNOWN_SAVE);
        if (why != nw_unknown)
        {
            if (why != NULL) nw_requester(why);
            else nw_connecting();
            return;
        }
    }
    nw_open_join();
}

/* Once a second while a join waits: associated with that SSID yet? */
static VOID nw_join_tick(VOID)
{
    char text[48];

    if (nw.join_wait <= 0) return;
    if (nw_joined())
    {
        nw.join_formless = FALSE;
        nw_close_join();
        nw_refresh_status();
        nw_known_fill();
        return;
    }
    if (--nw.join_wait == 0)
    {
        nw_known_joining();
        /* The saved key did not do: the form, to type a new one. */
        if (nw.join_formless && nw.join.win == NULL)
            nw_open_form(nw.join_ssid, nw.join_ssid_len, nw.join_open,
                         nw.join_hidden);
        nw.join_formless = FALSE;
        nw_join_status("Not connected after 30 seconds.");
        nw_refresh_status();
        nw_known_fill();
        return;
    }
    tool_copy_string(text, sizeof(text), "Waiting for the connection: ");
    nw_decimal(nw.join_wait, text + nw_len(text));
    nw_append(text, sizeof(text), " s");
    if (nw.join.win != NULL) nw_join_status(text);
    else nw_show_connecting(text);
}

/* -------------------------------------------------------- the icon -- */

static BPTR nw_icon_dir(char *name, ULONG len)
{
    char prog[108];

    if (nw.wbs != NULL && nw.wbs->sm_NumArgs > 0)
    {
        tool_copy_string(name, len, (const char *)nw.wbs->sm_ArgList[0].wa_Name);
        return nw.wbs->sm_ArgList[0].wa_Lock;
    }
    if (!GetProgramName((STRPTR)prog, sizeof(prog))) return (BPTR)0;
    tool_copy_string(name, len, (const char *)FilePart((STRPTR)prog));
    return GetProgramDir();
}

static struct DiskObject *nw_icon(VOID)
{
    char               name[108];
    BPTR               dir, old;
    struct DiskObject *dob;

    if (IconBase == NULL || (dir = nw_icon_dir(name, sizeof(name))) == (BPTR)0)
        return NULL;
    old = CurrentDir(dir);
    dob = GetDiskObject((STRPTR)name);
    (VOID)CurrentDir(old);
    return dob;
}

static const char *nw_tooltype(struct DiskObject *dob, const char *key)
{
    if (dob == NULL || dob->do_ToolTypes == NULL) return NULL;
    return (const char *)FindToolType((CONST_STRPTR *)dob->do_ToolTypes,
                                      (CONST_STRPTR)key);
}

static LONG nw_tooltype_number(struct DiskObject *dob, const char *key)
{
    const char *v = nw_tooltype(dob, key);
    LONG        n;

    return (v != NULL && StrToLong((CONST_STRPTR)v, &n) > 0 && n >= 0) ? n : -1;
}

static BOOL nw_is_key(const char *tt, const char *key)
{
    while (*key != '\0' && *tt == *key) { tt++; key++; }
    return *key == '\0' && (*tt == '=' || *tt == '\0');
}

/* LEFT and TOP into the icon, every other ToolType kept, as NetMeter's
   Snapshot does. */
static VOID nw_snapshot(VOID)
{
    static char   val[2][16];
    static STRPTR tt[35];
    char          name[108];
    BPTR          dir, old;
    struct DiskObject *dob;
    STRPTR       *was;
    UWORD         i, n = 0;
    BOOL          ok = FALSE;

    if (IconBase == NULL || (dir = nw_icon_dir(name, sizeof(name))) == (BPTR)0)
        return;
    old = CurrentDir(dir);
    dob = GetDiskObject((STRPTR)name);
    if (dob == NULL) dob = GetDefDiskObject(WBTOOL);
    if (dob != NULL)
    {
        was = dob->do_ToolTypes;
        for (i = 0; was != NULL && was[i] != NULL && n < 32; i++)
            if (!nw_is_key((const char *)was[i], "LEFT") &&
                !nw_is_key((const char *)was[i], "TOP"))
                tt[n++] = was[i];
        tool_copy_string(val[0], sizeof(val[0]), "LEFT=");
        nw_decimal(nw.main.win->LeftEdge, val[0] + 5);
        tool_copy_string(val[1], sizeof(val[1]), "TOP=");
        nw_decimal(nw.main.win->TopEdge, val[1] + 4);
        tt[n++] = (STRPTR)val[0];
        tt[n++] = (STRPTR)val[1];
        tt[n] = NULL;
        dob->do_ToolTypes = tt;
        ok = PutDiskObject((STRPTR)name, dob) ? TRUE : FALSE;
        dob->do_ToolTypes = was;
        FreeDiskObject(dob);
    }
    (VOID)CurrentDir(old);
    if (!ok) nw_requester("The position could not be saved in the icon.");
}

/* ------------------------------------------------------- the loop -- */

/* ----------------------------------------------------- known networks -- */

/* Save and Use wait while a join waits for its connection: either
   restarts WirelessManager under it. */
static VOID nw_known_joining(VOID)
{
    if (nw.known.win == NULL) return;
    nw_set(nw.known.win, NW_K_SAVE, GA_Disabled, nw.join_wait != 0);
    nw_set(nw.known.win, NW_K_USE, GA_Disabled, nw.join_wait != 0);
    nw_set(nw.known.win, NW_K_CONNECT, GA_Disabled, nw.join_wait != 0 || kn.sel < 0);
}

static VOID nw_known_fill(VOID)
{
    LONG i;

    if (nw.known.win == NULL) return;
    nw_set(nw.known.win, NW_K_LIST, GTLV_Labels, ~0UL);
    kn.list.lh_Head = (struct Node *)&kn.list.lh_Tail;
    kn.list.lh_Tail = NULL;
    kn.list.lh_TailPred = (struct Node *)&kn.list.lh_Head;
    for (i = 0; i < kn.shown; i++)
    {
        const NwNetwork *n = &kn.nets[kn.order[i]];
        char            *t = kn.name[i];
        char             mark[40], name[NW_SSID_MAX + 1];
        ULONG            room;

        /* Latin-1 has no ellipsis character: three full stops. */
        mark[0] = '\0';
        if (n->scan_ssid) nw_append(mark, sizeof(mark), " (hidden)");
        if (n->disabled) nw_append(mark, sizeof(mark), " (off)");
        if (nw.join_wait > 0 &&
            nw_same(n->ssid, n->ssid_len, nw.join_ssid, nw.join_ssid_len))
            nw_append(mark, sizeof(mark), " (connecting...)");
        else if (nw_same(n->ssid, n->ssid_len, nw.conn_ssid, nw.conn_len))
            nw_append(mark, sizeof(mark), " (connected)");
        if (n->ssid_len >= 0) nw_ssid_text(n->ssid, n->ssid_len, name);
        else tool_copy_string(name, sizeof(name), "?");
        /* The name shortened, never the markers. */
        room = sizeof(kn.name[0]) - 1 - nw_len(mark);
        if (nw_len(name) > room) name[room] = '\0';
        tool_copy_string(t, sizeof(kn.name[0]), name);
        nw_append(t, sizeof(kn.name[0]), mark);
        kn.node[i].ln_Name = t;
        AddTail(&kn.list, &kn.node[i]);
    }
    {
        /* The selection kept in sight: GTLV_MakeVisible from V39, the top
           row moved before that. */
        struct TagItem t[4];

        t[0].ti_Tag = GTLV_Labels;   t[0].ti_Data = (ULONG)&kn.list;
        t[1].ti_Tag = GTLV_Selected; t[1].ti_Data = kn.sel >= 0 ? (ULONG)kn.sel : ~0UL;
        t[2].ti_Tag = TAG_DONE;
        if (kn.sel >= 0)
        {
            t[2].ti_Tag = GadToolsBase->lib_Version >= 39 ? GTLV_MakeVisible : GTLV_Top;
            t[2].ti_Data = GadToolsBase->lib_Version >= 39 || kn.sel < NW_LIST_ROWS
                           ? (ULONG)(GadToolsBase->lib_Version >= 39 ? kn.sel : 0)
                           : (ULONG)(kn.sel - NW_LIST_ROWS + 1);
            t[3].ti_Tag = TAG_DONE;
        }
        GT_SetGadgetAttrsA(nw.g[NW_K_LIST], nw.known.win, NULL, t);
    }
    nw_set(nw.known.win, NW_K_UP, GA_Disabled, kn.sel <= 0);
    nw_set(nw.known.win, NW_K_DOWN, GA_Disabled, kn.sel < 0 || kn.sel + 1 >= kn.shown);
    nw_set(nw.known.win, NW_K_REMOVE, GA_Disabled, kn.sel < 0);
    nw_known_joining();
}

static VOID nw_close_known(VOID)
{
    int i;

    if (nw.known.win != NULL) CloseWindow(nw.known.win);
    if (nw.known.glist != NULL) FreeGadgets(nw.known.glist);
    nw.known.win = NULL;
    nw.known.glist = NULL;
    for (i = NW_K_LIST; i <= NW_K_CONNECT; i++) nw.g[i] = NULL;
    nw_free_file(kn.text, kn.len);
    kn.text = NULL;
}

/* Wireless.prefs read, its networks highest priority first (file order
   among equals).  FALSE, said, when it cannot be edited. */
static BOOL nw_known_load(VOID)
{
    BOOL missing;
    LONG i;

    nw_free_file(kn.text, kn.len);
    kn.text = nw_read_prefs(&kn.len, &missing);
    if (kn.text == NULL && !missing)
    {
        nw_requester("Wireless.prefs could not be read.");
        return FALSE;
    }
    kn.count = nw_prefs_parse(kn.text != NULL ? kn.text : "", kn.len, kn.nets,
                              NW_NETS_MAX);
    if (kn.count < 0 || kn.count > NW_NETS_MAX)
    {
        nw_requester(kn.count < 0 ? "Wireless.prefs has a network={ with no closing }."
                                  : "Wireless.prefs has more networks than this edits.");
        return FALSE;
    }
    nw_prefs_order(kn.nets, (int)kn.count, kn.order);
    for (i = 0; i < kn.count; i++) kn.was[i] = kn.order[i];
    kn.shown = kn.count;
    kn.sel = -1;
    kn.sel_fresh = FALSE;
    return TRUE;
}

/* The window opened on Wireless.prefs' networks. */
static VOID nw_open_known(VOID)
{
    struct Gadget *g;
    struct TagItem t[3];

    if (nw.known.win != NULL)
    {
        WindowToFront(nw.known.win);
        ActivateWindow(nw.known.win);
        return;
    }
    kn.text = NULL;
    if (!nw_known_load())
    {
        nw_close_known();
        return;
    }

    g = CreateContext(&nw.known.glist);
    kn.list.lh_Head = (struct Node *)&kn.list.lh_Tail;
    kn.list.lh_TailPred = (struct Node *)&kn.list.lh_Head;
    t[0].ti_Tag = GTLV_Labels;       t[0].ti_Data = (ULONG)&kn.list;
    t[1].ti_Tag = GTLV_ShowSelected; t[1].ti_Data = 0;
    t[2].ti_Tag = TAG_DONE;
    g = nw_gadget(LISTVIEW_KIND, g, NW_K_LIST, t);
    g = nw_gadget(BUTTON_KIND, g, NW_K_UP, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_K_DOWN, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_K_REMOVE, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_K_CONNECT, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_K_SAVE, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_K_USE, NULL);
    g = nw_gadget(BUTTON_KIND, g, NW_K_CANCEL, NULL);
    if (g == NULL ||
        (nw.known.win = nw_open(NW_WIN_KNOWN, nw.known.glist, "Known Wi-Fi networks",
                                (NW_IDCMP & ~IDCMP_MENUPICK) | IDCMP_VANILLAKEY,
                                -1, -1)) == NULL)
    {
        nw_close_known();
        return;
    }
    GT_RefreshWindow(nw.known.win, NULL);
    nw_known_fill();
}

static VOID nw_known_move(LONG to)
{
    UBYTE b = kn.order[kn.sel];

    kn.order[kn.sel] = kn.order[to];
    kn.order[to] = b;
    kn.sel = to;
    kn.sel_fresh = FALSE;
    nw_known_fill();
}

static VOID nw_known_remove(VOID)
{
    LONG i;

    for (i = kn.sel; i + 1 < kn.shown; i++) kn.order[i] = kn.order[i + 1];
    kn.shown--;
    if (kn.sel >= kn.shown) kn.sel = kn.shown - 1;
    kn.sel_fresh = FALSE;
    nw_known_fill();
}

#define NW_KS_USE     0
#define NW_KS_SAVE    1
#define NW_KS_CONNECT 2

/* Save (ENVARC: and ENV:) or Use (ENV:): priorities from the order shown,
   the top one highest, removed blocks taken out, everything else kept;
   then WirelessManager, if it runs, restarted on the new file.  Connect,
   and Use with a network picked that is not the one in use, nothing
   moved or removed since the pick: that network moved to the top and
   joined (Save only stores).  Connect writes ENVARC: too, as Join does,
   and keeps the window open on the new file. */
static VOID nw_known_store(int how)
{
    const char *why = NULL;
    BOOL        moved = kn.shown != kn.count, connect = FALSE;
    LONG        n, at = -1;

    if (nw.join_wait != 0) return;
    if (kn.sel >= 0 && how != NW_KS_SAVE)
    {
        const NwNetwork *e = &kn.nets[kn.order[kn.sel]];

        connect = e->ssid_len > 0 && !e->disabled &&
                  (how == NW_KS_CONNECT ||
                   (kn.sel_fresh &&
                    !nw_same(e->ssid, e->ssid_len, nw.conn_ssid, nw.conn_len)));
        /* Nothing to join with: the form, for its passphrase; its Join
           puts the network on top.  Nothing is written before that. */
        if (connect && !e->has_psk && !e->open && !e->wep)
        {
            UBYTE ssid[NW_SSID_MAX];
            LONG  len = e->ssid_len, i;
            BOOL  hidden = e->scan_ssid != 0;

            /* Use keeps what it says: an order edited before the pick is
               stored (ENV:) first.  Connect stores nothing. */
            for (i = 0; i < len; i++) ssid[i] = e->ssid[i];
            if (how == NW_KS_USE)
            {
                for (n = 0; n < kn.shown && kn.order[n] == kn.was[n]; n++)
                    ;
                if ((n < kn.shown || kn.shown != kn.count) &&
                    (why = nw_store_order(kn.text, kn.len, kn.nets, kn.count,
                                          kn.order, kn.shown, FALSE)) != NULL)
                {
                    nw_requester(why);
                    return;
                }
                nw_close_known();
            }
            nw.join_hidden = hidden;
            nw_open_form(ssid, len, FALSE, hidden);
            return;
        }
        if (connect)
        {
            at = kn.order[kn.sel];
            for (n = kn.sel; n > 0; n--) kn.order[n] = kn.order[n - 1];
            kn.order[0] = (UBYTE)at;
            kn.sel = 0;
        }
    }
    /* Nothing moved or removed, in the end: nothing written. */
    for (n = 0; !moved && n < kn.shown; n++)
        if (kn.order[n] != kn.was[n]) moved = TRUE;
    if (!moved && !connect)
    {
        nw_close_known();
        return;
    }
    /* A Join since this window opened changed the file: keep that. */
    {
        ULONG now_len;
        BOOL  missing;
        char *now = nw_read_prefs(&now_len, &missing);
        BOOL  same = (now == NULL && kn.text == NULL) ||
                     (now != NULL && kn.text != NULL && now_len == kn.len);
        ULONG i;

        for (i = 0; same && now != NULL && i < now_len; i++)
            if (now[i] != kn.text[i]) same = FALSE;
        nw_free_file(now, now_len);
        if (!same)
        {
            nw_requester("Wireless.prefs changed since this window opened, "
                         "so it was not written. Open Known networks again.");
            nw_close_known();
            return;
        }
    }
    if (moved)
        why = nw_store_order(kn.text, kn.len, kn.nets, kn.count, kn.order,
                             kn.shown, how != NW_KS_USE);
    if (why != NULL)
    {
        nw_requester(why);
        return;
    }
    if (connect)
    {
        const NwNetwork *e = &kn.nets[at];

        nw.join_hidden = e->scan_ssid != 0;
        nw.join_open = e->open != 0;
        why = nw_start_join(e->ssid, e->ssid_len);
        if (how == NW_KS_CONNECT && nw_known_load())
            kn.sel = 0;
        else
            nw_close_known();
        if (why != NULL) nw_requester(why);
        else nw_connecting();
        return;
    }
    nw_close_known();
    if (nw_wm_running(NULL, 0) && (why = nw_restart_wm()) != NULL)
        nw_requester(why);
    nw_rank_scan();
    nw_fill_list();
    nw_refresh_status();
}

static VOID nw_timer_arm(VOID)
{
    if (nw.treq == NULL || nw.tarmed) return;
    nw.treq->tr_node.io_Command = TR_ADDREQUEST;
    nw.treq->tr_time.tv_secs = 1;
    nw.treq->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)nw.treq);
    nw.tarmed = TRUE;
}

static VOID nw_cancel_join(VOID)
{
    nw_close_join();
    if (nw.setup && !nw.join_saved) nw_finish(NW_SETUP_CANCELLED);
}

static VOID nw_gadget_up(const struct IntuiMessage *m)
{
    struct Gadget *g = (struct Gadget *)m->IAddress;
    UWORD          code = m->Code;

    switch (g->GadgetID)
    {
        case NW_M_LIST:
            if ((LONG)code == nw.selected && nw.dev_open &&
                DoubleClick(nw.click_secs, nw.click_micros, m->Seconds, m->Micros))
                nw_join_pressed();
            nw.selected = (LONG)code < nw.count ? (LONG)code : -1;
            nw.sel_len = -1;
            if (nw.selected >= 0)
            {
                nw.sel_len = nw.nets[nw.selected].ssid_len;
                CopyMem(nw.nets[nw.selected].ssid, nw.sel_ssid, (ULONG)nw.sel_len);
            }
            nw.click_secs = m->Seconds;
            nw.click_micros = m->Micros;
            break;
        case NW_M_SCAN: nw_scan(); break;
        case NW_M_JOIN: nw_join_pressed(); break;
        case NW_M_DISCONNECT:
            nw.wm_seen = TRUE;
            nw_wm_mark_off();
            (VOID)nw_wm_stop();
            if (nw.join_formless)
            {
                nw.join_formless = FALSE;
                nw.join_wait = 0;
                nw_known_joining();
                nw_known_fill();
            }
            nw_refresh_status();
            break;
        case NW_J_SECURITY:
            nw.open_network = code == 1;
            if (nw.open_network) nw_wipe_pass();
            nw_set(nw.join.win, NW_J_PASS, GA_Disabled, nw.open_network);
            break;
        case NW_J_SSID:
        {
            /* A known name typed: its saved key is offered. */
            struct StringInfo *si = (struct StringInfo *)g->SpecialInfo;
            UBYTE ssid[NW_SSID_MAX];
            LONG  len = nw_form_ssid((const char *)si->Buffer, ssid);
            BOOL  saved = !nw.open_network && nw.pass_len == 0 && len > 0 &&
                          nw_known_network(ssid, len, NW_KNOWN_CHECK) == NULL;

            if (saved != nw.pass_saved)
            {
                nw.pass_saved = saved;
                nw_set(nw.join.win, NW_J_PASS, GTST_String,
                       (ULONG)(saved ? nw_saved_text : ""));
                nw_join_status(saved ? nw_saved_note : "");
            }
            break;
        }
        case NW_J_SHOW:
        {
            static char stars[NW_PASS_MAX + 1];
            int i;

            nw.pass_shown = (g->Flags & GFLG_SELECTED) != 0;
            if (nw.pass_saved) break;
            for (i = 0; i < nw.pass_len; i++) stars[i] = '*';
            stars[i] = '\0';
            nw_set(nw.join.win, NW_J_PASS, GTST_String,
                   (ULONG)(nw.pass_shown ? nw.pass : stars));
            break;
        }
        case NW_J_JOIN:   nw_join(); break;
        case NW_J_CANCEL: nw_cancel_join(); break;
        case NW_M_KNOWN:  nw_open_known(); break;
        case NW_K_LIST:
            if ((LONG)code == kn.sel &&
                DoubleClick(kn.click_secs, kn.click_micros, m->Seconds, m->Micros))
            {
                nw_known_store(NW_KS_CONNECT);
                break;
            }
            kn.sel = (LONG)code < kn.shown ? (LONG)code : -1;
            kn.sel_fresh = kn.sel >= 0;
            kn.click_secs = m->Seconds;
            kn.click_micros = m->Micros;
            nw_known_fill();
            break;
        case NW_K_UP:     if (kn.sel > 0) nw_known_move(kn.sel - 1); break;
        case NW_K_DOWN:   if (kn.sel >= 0 && kn.sel + 1 < kn.shown) nw_known_move(kn.sel + 1); break;
        case NW_K_REMOVE: if (kn.sel >= 0) nw_known_remove(); break;
        case NW_K_SAVE:    nw_known_store(NW_KS_SAVE); break;
        case NW_K_USE:     nw_known_store(NW_KS_USE); break;
        case NW_K_CONNECT: if (kn.sel >= 0) nw_known_store(NW_KS_CONNECT); break;
        case NW_K_CANCEL: nw_close_known(); break;
        default: break;
    }
}

static VOID nw_menu_pick(UWORD n)
{
    while (n != MENUNULL && !nw.done)
    {
        struct MenuItem *item = ItemAddress(nw.menu, n);

        if (item == NULL) break;
        switch ((ULONG)GTMENUITEM_USERDATA(item))
        {
            case MENU_SCAN:     nw_scan(); break;
            case MENU_JOIN:     if (nw.dev_open) nw_join_pressed(); break;
            case MENU_KNOWN:    nw_open_known(); break;
            case MENU_SNAPSHOT: nw_snapshot(); break;
            case MENU_QUIT:     nw_finish(RETURN_OK); break;
            default: break;
        }
        n = item->NextSelect;
    }
}

/* One window's messages.  The join window can close under a message, so
   its port is not touched after that. */
static VOID nw_window_msgs(NwWin *w)
{
    struct IntuiMessage *m;

    while (w->win != NULL && (m = GT_GetIMsg(w->win->UserPort)) != NULL)
    {
        struct IntuiMessage copy = *m;

        GT_ReplyIMsg(m);
        switch (copy.Class)
        {
            case IDCMP_CLOSEWINDOW:
                if (w == &nw.join) nw_cancel_join();
                else if (w == &nw.known) nw_close_known();
                else nw_finish(nw.setup ? NW_SETUP_CANCELLED : RETURN_OK);
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(w->win);
                GT_EndRefresh(w->win, TRUE);
                break;
            case IDCMP_GADGETUP:  nw_gadget_up(&copy); break;
            case IDCMP_MENUPICK:  nw_menu_pick(copy.Code); break;
            case IDCMP_VANILLAKEY:              /* Esc is Cancel */
                if (w == &nw.known && copy.Code == 0x1b) nw_close_known();
                break;
            default: break;
        }
    }
}

static VOID nw_loop(VOID)
{
    while (!nw.done)
    {
        ULONG main_sig = 1UL << nw.main.win->UserPort->mp_SigBit;
        ULONG join_sig = nw.join.win != NULL ? 1UL << nw.join.win->UserPort->mp_SigBit : 0;
        ULONG known_sig = nw.known.win != NULL ? 1UL << nw.known.win->UserPort->mp_SigBit : 0;
        ULONG scan_sig = nw.dev_open ? 1UL << nw.dev.async->mp_SigBit : 0;
        ULONG time_sig = nw.tport != NULL ? 1UL << nw.tport->mp_SigBit : 0;
        ULONG sigs = Wait(main_sig | join_sig | known_sig | scan_sig | time_sig |
                          SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F);

        if (sigs & SIGBREAKF_CTRL_C)
            nw_finish(nw.setup ? NW_SETUP_CANCELLED : RETURN_OK);
        if (sigs & SIGBREAKF_CTRL_F)
        {
            /* Another start of this program: this window to the front. */
            WindowToFront(nw.main.win);
            ActivateWindow(nw.main.win);
        }
        if ((sigs & scan_sig) && nw_dev_reply(&nw.dev) == NW_REPLY_SCAN)
            nw_scan_done();
        if ((sigs & time_sig) && GetMsg(nw.tport) != NULL)
        {
            nw.tarmed = FALSE;
            nw_join_tick();
            if (++nw.ticks % NW_REFRESH == 0 && nw.join_wait == 0)
                nw_refresh_status();
            nw_timer_arm();
        }
        if (sigs & join_sig) nw_window_msgs(&nw.join);
        if (sigs & known_sig) nw_window_msgs(&nw.known);
        if (sigs & main_sig) nw_window_msgs(&nw.main);
    }
}

/* ------------------------------------------------------ the window -- */

static int nw_window(VOID)
{
    static char     port_name[] = "NetWirelessPrefs";
    struct MsgPort *me;

    /* One window: a second start brings the first to the front. */
    Forbid();
    me = FindPort((CONST_STRPTR)port_name);
    if (me != NULL) Signal(me->mp_SigTask, SIGBREAKF_CTRL_F);
    Permit();
    if (me != NULL) return nw.setup ? NW_SETUP_CANCELLED : RETURN_OK;
    me = CreateMsgPort();
    if (me == NULL) return RETURN_FAIL;
    me->mp_Node.ln_Name = port_name;
    AddPort(me);

    nw.result = RETURN_FAIL;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase = OpenLibrary("gadtools.library", 37);
    if (IntuitionBase != NULL && GfxBase != NULL && GadToolsBase != NULL &&
        (nw.screen = LockPubScreen(NULL)) != NULL &&
        !nw_layout_in(nw.screen->Font) && !nw_layout_in(&nw_topaz8))
        nw_requester("The window does not fit on this screen.");
    else if (nw.screen != NULL &&
             (nw.visual = GetVisualInfoA(nw.screen, NULL)) != NULL)
    {
        nw.tport = CreateMsgPort();
        nw.treq = (struct timerequest *)CreateIORequest(nw.tport, sizeof(struct timerequest));
        if (nw.treq != NULL &&
            OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                       (struct IORequest *)nw.treq, 0) != 0)
        {
            DeleteIORequest((struct IORequest *)nw.treq);
            nw.treq = NULL;
        }
        nw.dev_open = nw_dev_open(&nw.dev, nw.device, nw.unit) == 0;
        if (!nw.dev_open && nw.setup)
            nw.result = NW_SETUP_NO_DEVICE;
        else if (nw_open_main())
        {
            /* No unit, or no pools to scan with: nothing to scan or join. */
            BOOL can_scan = nw.dev_open && SysBase->LibNode.lib_Version >= 39;

            nw_set(nw.main.win, NW_M_SCAN, GA_Disabled, !can_scan);
            nw_set(nw.main.win, NW_M_JOIN, GA_Disabled, !nw.dev_open);
            nw_refresh_status();
            nw_fill_list();
            if (can_scan) nw_scan();
            nw_timer_arm();
            nw_loop();
        }
        else
            nw_requester("The window could not be opened.");
    }

    nw_close_join();
    nw_close_known();
    if (nw.main.win != NULL)
    {
        if (nw.menu != NULL) ClearMenuStrip(nw.main.win);
        CloseWindow(nw.main.win);
    }
    if (nw.menu != NULL) FreeMenus(nw.menu);
    if (nw.main.glist != NULL) FreeGadgets(nw.main.glist);
    if (nw.dev_open) nw_dev_close(&nw.dev);
    if (nw.tarmed)
    {
        AbortIO((struct IORequest *)nw.treq);
        WaitIO((struct IORequest *)nw.treq);
    }
    if (nw.treq != NULL)
    {
        CloseDevice((struct IORequest *)nw.treq);
        DeleteIORequest((struct IORequest *)nw.treq);
    }
    if (nw.tport != NULL) DeleteMsgPort(nw.tport);
    if (nw.visual != NULL) FreeVisualInfo(nw.visual);
    if (nw.font != NULL) CloseFont(nw.font);
    if (nw.screen != NULL) UnlockPubScreen(NULL, nw.screen);
    if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
    if (GfxBase != NULL) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
    RemPort(me);
    DeleteMsgPort(me);
    return nw.result;
}

/*
 * Intuition, GadTools and the key derivation run on the caller's stack, and
 * a Shell gives a command 4 KB.  Below 8 KB the window runs on a 16 KB stack
 * of its own, the trampoline NetPrefs uses; nw_trampoline() has no locals.
 */
#define NW_STACK_SIZE (16UL * 1024UL)
#define NW_SAFE_STACK (8UL * 1024UL)

static struct StackSwapStruct nw_sss;
static int                    nw_window_rc;

static __attribute__((noinline)) VOID nw_trampoline(VOID)
{
    StackSwap(&nw_sss);
    nw_window_rc = nw_window();
    StackSwap(&nw_sss);
}

static int nw_window_on_stack(VOID)
{
    struct Task *me = FindTask(NULL);
    APTR         stack;

    if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= NW_SAFE_STACK)
        return nw_window();
    stack = AllocMem(NW_STACK_SIZE, MEMF_ANY);
    if (stack == NULL) return RETURN_FAIL;
    nw_sss.stk_Lower   = stack;
    nw_sss.stk_Upper   = (ULONG)stack + NW_STACK_SIZE;
    nw_sss.stk_Pointer = (APTR)((ULONG)stack + NW_STACK_SIZE);
    nw_trampoline();
    FreeMem(stack, NW_STACK_SIZE);
    return nw_window_rc;
}

int main(int argc, char **argv)
{
    LONG               args[ARG_COUNT] = { 0 };
    struct RDArgs     *rda = NULL;
    struct DiskObject *dob;
    const char        *wm, *iface;
    LONG               rc = RETURN_OK, err;
    BOOL               known;

    nw.selected = -1;
    nw.conn_len = -1;
    nw.sel_len = -1;
    IconBase = OpenLibrary("icon.library", 37);
    if (argc == 0)
        nw.wbs = (struct WBStartup *)argv;
    else
    {
        tool_break_arm();
        rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
        if (rda == NULL)
        {
            tool_fault(IoErr());
            tool_usage("[DEVICE <name>] [UNIT <n>] [INTERFACE <name>] "
                       "[WM <command>] [LIST] [STATUS] [SETUP [TO <file>]] [DISCONNECT] "
                       "[JOIN <ssid> [PASSPHRASE <text>] [HIDDEN]]",
                       "Join a Wi-Fi network, or list the networks in range.");
            if (IconBase != NULL) CloseLibrary(IconBase);
            return RETURN_ERROR;
        }
    }

    /* Arguments win over the icon's ToolTypes. */
    dob = nw_icon();
    iface = args[ARG_INTERFACE] != 0 ? (const char *)args[ARG_INTERFACE]
                                     : nw_tooltype(dob, "INTERFACE");
    known = nw_pick_device(args[ARG_DEVICE] != 0 ? (const char *)args[ARG_DEVICE]
                                                 : nw_tooltype(dob, "DEVICE"),
                           args[ARG_UNIT] != 0 ? *(LONG *)args[ARG_UNIT]
                                               : nw_tooltype_number(dob, "UNIT"),
                           iface);
    wm = args[ARG_WM] != 0 ? (const char *)args[ARG_WM] : nw_tooltype(dob, "WM");
    tool_copy_string(nw.wm, sizeof(nw.wm), wm != NULL ? wm : NW_WM_COMMAND);
    nw.want_left = (WORD)nw_tooltype_number(dob, "LEFT");
    nw.want_top = (WORD)nw_tooltype_number(dob, "TOP");
    nw.setup = args[ARG_SETUP] != 0;
    if (nw.setup && args[ARG_TO] != 0)
        tool_copy_string(nw.to, sizeof(nw.to), (const char *)args[ARG_TO]);
    if (dob != NULL) FreeDiskObject(dob);

    /* Only WirelessManager is stopped: no Wi-Fi unit is needed. */
    if (args[ARG_DISCONNECT] != 0)
    {
        /* As the window's Disconnect: WirelessManager stopped. */
        if (!nw_wm_running(NULL, 0))
            tool_printf("WirelessManager is not running.\n");
        else if (!nw_wm_stop())
        {
            tool_error("WirelessManager did not stop");
            rc = RETURN_ERROR;
        }
        else
            tool_printf("Disconnected.\n");
        if (rc == RETURN_OK) nw_wm_mark_off();
    }
    else if (!known)
    {
        if (rda != NULL)
            tool_error("DEVS:NetInterfaces/%s names no DEVICE", (LONG)iface);
        rc = RETURN_ERROR;              /* NW_SETUP_NO_DEVICE for SETUP */
    }
    else if (args[ARG_LIST] != 0 || args[ARG_STATUS] != 0 || args[ARG_JOIN] != 0)
    {
        err = nw_dev_open(&nw.dev, nw.device, nw.unit);
        nw.dev_open = err == 0;
        if (err != 0)
        {
            tool_error("cannot open %s unit %lu (error %ld)", (LONG)nw.device,
                       (LONG)nw.unit, err);
            rc = RETURN_ERROR;
        }
        else
        {
            if (args[ARG_JOIN] != 0)
            {
                char *p = (char *)args[ARG_PASSPHRASE];

                rc = nw_shell_join((const char *)args[ARG_JOIN], p,
                                   args[ARG_HIDDEN] != 0);
                while (p != NULL && *p != '\0') *p++ = '\0';
            }
            else
                rc = args[ARG_LIST] != 0 ? nw_shell_list() : nw_shell_status();
            nw_dev_close(&nw.dev);
        }
    }
    else
        rc = nw_window_on_stack();

    if (rda != NULL) FreeArgs(rda);
    if (IconBase != NULL) CloseLibrary(IconBase);
    return (int)rc;
}
