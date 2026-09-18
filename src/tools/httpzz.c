/*
 * ZZ9000 console-encode offload -- the host half.  See httpzz.h.
 *
 * zz9k.library ships no m68k inline glue (there is no inline/zz9k.h), so the
 * handful of calls used here are hand-declared against inline/macros.h with the
 * offsets from third_party/zz9000-sdk/amiga/fd/zz9k_lib.fd (##bias 30, six
 * bytes per vector) -- exactly the way httprtg.c reaches Picasso96 and
 * CyberGraphX.  The library is opened by name; a failure to open is the
 * fallback, not an error.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "httpzz.h"

#include <exec/libraries.h>
#include <proto/exec.h>
#include <inline/macros.h>

/* Structure and constant definitions come from the submodule, which compiles
   clean on m68k (data only; the function prototypes there are unused -- the
   calls are the LP wrappers below). */
#include "zz9k/abi.h"
#include "zz9k/host.h"
#include "zz9k/caps.h"
#include "zz9k/surface.h"

#define ZZ9K_LIB_NAME   "zz9k.library"
#define ZZ9K_MIN_VER    2

/* fd vector offsets: 0x1e + 6*(index-1).  Kept as the documented vector table;
   the LP wrappers below must pass the number as a literal, because LP stringizes
   its offset argument (jsr a6@(-"#offs":W)) and a macro name would not expand --
   so each literal carries the name in a trailing comment. */
#define ZZLVO_QUERY_CAPS        0x1e
#define ZZLVO_QUERY_SERVICE     0x24
#define ZZLVO_CALL              0x30
#define ZZLVO_ALLOC_SHARED      0x48
#define ZZLVO_FREE_SHARED       0x4e
#define ZZLVO_MAP_FB_SURFACE    0x72

static struct Library *zz_base;
static int             zz_probed;      /* 0 unknown, 1 available, -1 no */

static ZZ9KSharedBuffer zz_out;        /* the encoder's output, allocated once */
static ULONG            zz_out_cap;    /* its length, 0 when none              */
static UWORD            zz_reset;       /* next encode drops the delta baseline */

/* ----------------------------------------------------- library vectors --- */

static int zz_query_caps(ZZ9KCaps *caps)
{
    return LP1(0x1e /* QUERY_CAPS */, int, ZZ9KQueryCaps,
               ZZ9KCaps *, caps, a0,
               , zz_base);
}

static int zz_query_service(ULONG id, ZZ9KServiceInfo *si)
{
    return LP2(0x24 /* QUERY_SERVICE */, int, ZZ9KQueryService,
               ULONG, id, d0, ZZ9KServiceInfo *, si, a0,
               , zz_base);
}

static int zz_call(ZZ9KRequest *req, ZZ9KMailboxEntry *reply, ULONG timeout)
{
    return LP3(0x30 /* CALL */, int, ZZ9KCall,
               ZZ9KRequest *, req, a0, ZZ9KMailboxEntry *, reply, a1,
               ULONG, timeout, d0,
               , zz_base);
}

static int zz_alloc_shared(ULONG len, ULONG align, ULONG flags,
                           ZZ9KSharedBuffer *buf)
{
    return LP4(0x48 /* ALLOC_SHARED */, int, ZZ9KAllocShared,
               ULONG, len, d0, ULONG, align, d1, ULONG, flags, d2,
               ZZ9KSharedBuffer *, buf, a0,
               , zz_base);
}

static int zz_free_shared(ULONG handle)
{
    return LP1(0x4e /* FREE_SHARED */, int, ZZ9KFreeShared,
               ULONG, handle, d0,
               , zz_base);
}

static int zz_map_fb(ZZ9KSurface *surf)
{
    return LP1(0x72 /* MAP_FB_SURFACE */, int, ZZ9KMapFramebufferSurface,
               ZZ9KSurface *, surf, a0,
               , zz_base);
}

/* --------------------------------------------------------- detection ---- */

BOOL httpzz_available(VOID)
{
    ZZ9KCaps        caps;
    ZZ9KServiceInfo si;

    if (zz_probed != 0)
        return (zz_probed > 0) ? TRUE : FALSE;

    zz_probed = -1;                     /* until proven otherwise */

    zz_base = OpenLibrary((CONST_STRPTR)ZZ9K_LIB_NAME, ZZ9K_MIN_VER);
    if (zz_base == NULL)
        return FALSE;                   /* no card, or pre-SDK-v2 firmware */

    /* The board must expose its framebuffer as a surface -- without that the
       encoder cannot read the screen locally. */
    memset(&caps, 0, sizeof(caps));
    if (zz_query_caps(&caps) != ZZ9K_STATUS_OK ||
        !zz9k_has_capability(caps.capability_bits, ZZ9K_CAP_FRAMEBUFFER_SURFACE))
        goto no;

    /* The console-encode vendor service must be loaded.  Not found is
       "feature absent", per the vendor-service contract -- fall back. */
    memset(&si, 0, sizeof(si));
    if (zz_query_service(HTTPZZ_SERVICE_ID, &si) != ZZ9K_STATUS_OK)
        goto no;

    zz_probed = 1;
    return TRUE;

no:
    CloseLibrary(zz_base);
    zz_base = NULL;
    return FALSE;
}

/* --------------------------------------------------------- the encode --- */

/* Grow the shared output buffer to at least `need` bytes.  Returns TRUE with
   zz_out valid, or FALSE. */
static BOOL zz_out_ensure(ULONG need)
{
    if (zz_out_cap >= need && zz_out.data != NULL)
        return TRUE;

    if (zz_out_cap != 0)
    {
        (VOID)zz_free_shared(zz_out.handle);
        zz_out_cap = 0;
        memset(&zz_out, 0, sizeof(zz_out));
    }

    if (zz_alloc_shared(need, 16UL, 0UL, &zz_out) != ZZ9K_STATUS_OK ||
        zz_out.data == NULL)
    {
        zz_out_cap = 0;
        return FALSE;
    }

    zz_out_cap = need;
    return TRUE;
}

LONG httpzz_encode(UWORD ty0, UWORD ty1, UBYTE *out, ULONG out_max,
                   UWORD *codec_out)
{
    ZZ9KSurface       fb;
    ZZ9KRequest       req;
    ZZ9KMailboxEntry  reply;
    HttpZzEncodeReq   er;
    HttpZzEncodeReply rr;
    ULONG             n;

    if (codec_out != NULL)
        *codec_out = HTTPZZ_CODEC_NONE;

    if (zz_base == NULL && !httpzz_available())
        return -1;

    /* The displayed framebuffer surface can change under a mode switch, so it
       is mapped afresh each band; the call just hands back the current one. */
    memset(&fb, 0, sizeof(fb));
    if (zz_map_fb(&fb) != ZZ9K_STATUS_OK)
        return -1;

    if (!zz_out_ensure(out_max))
        return -1;

    memset(&er, 0, sizeof(er));
    er.surface_handle = fb.handle;
    er.out_handle     = zz_out.handle;
    er.out_capacity   = zz_out_cap;
    er.ty0   = ty0;
    er.ty1   = ty1;
    er.codec = HTTPZZ_CODEC_NONE;
    er.flags = (UWORD)(zz_reset ? HTTPZZ_F_RESET : 0U);

    memset(&req, 0, sizeof(req));
    req.entry.opcode      = (UWORD)HTTPZZ_OP_ENCODE;
    req.entry.payload_len = (UWORD)sizeof(er);
    memcpy(req.entry.payload.inline_data, &er, sizeof(er));

    memset(&reply, 0, sizeof(reply));
    if (zz_call(&req, &reply, 0UL) != ZZ9K_STATUS_OK ||
        reply.status != ZZ9K_STATUS_OK)
        return -1;

    memcpy(&rr, reply.payload.inline_data, sizeof(rr));
    n = rr.out_len;
    if (n > zz_out_cap || n > out_max)
        return -1;

    if (n != 0UL)
        memcpy(out, (const void *)zz_out.data, n);
    if (codec_out != NULL)
        *codec_out = rr.codec;

    zz_reset = 0;                       /* the baseline is current again */
    return (LONG)n;
}

VOID httpzz_reset(VOID)
{
    zz_reset = 1;
}

VOID httpzz_cleanup(VOID)
{
    if (zz_base == NULL)
        return;

    if (zz_out_cap != 0)
    {
        (VOID)zz_free_shared(zz_out.handle);
        zz_out_cap = 0;
        memset(&zz_out, 0, sizeof(zz_out));
    }

    CloseLibrary(zz_base);
    zz_base   = NULL;
    zz_probed = 0;
}
