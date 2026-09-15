/*
 * NetDevStats: every counter a SANA-II driver keeps, printed raw.
 *
 * S2_GETGLOBALSTATS is the standard block; S2_GETSPECIALSTATS is whatever
 * the driver chose to count, each record carrying its own name.  ShowNetStatus
 * shows the handful it has names for; this shows all of them, for any driver,
 * which is what a new chip core is debugged from.  Opens the unit beside the
 * stack (a SANA-II unit takes many openers) and asks; no stack needed.
 */
#include "tools.h"

#include <exec/io.h>
#include <exec/memory.h>

#include "sana2_device.h"

#include "aminetxduo/anxnet.h"
#include "aminetxduo/compat.h"

const char *const tool_name = "NetDevStats";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetDevStats");

#define TEMPLATE    "DEVICE/K,UNIT/N/K,CARD/K"

enum
{
    ARG_DEVICE = 0,
    ARG_UNIT,
    ARG_CARD,
    ARG_COUNT
};

#define NDS_RECORDS 64

/* The driver copies nothing through this: no CMD_READ is ever queued. */
static BOOL nds_copy(register APTR to __asm("a0"), register APTR from __asm("a1"),
                     register ULONG n __asm("d0"))
{
    (VOID)to;
    (VOID)from;
    (VOID)n;
    return FALSE;
}

int main(int argc, char **argv)
{
    LONG               args[ARG_COUNT];
    struct RDArgs     *rda;
    const char        *device = ANXNET_DEVICE_NAME;
    const char        *card   = NULL;
    ULONG              unit   = 0;
    struct MsgPort    *port;
    struct IOSana2Req *req;
    struct TagItem     tags[4];
    UWORD              tag = 0;
    LONG               rc = RETURN_OK;
    struct
    {
        struct Sana2SpecialStatHeader hdr;
        struct Sana2SpecialStatRecord rec[NDS_RECORDS];
    } *special;
    struct Sana2DeviceStats *global;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    args[ARG_DEVICE] = 0;
    args[ARG_UNIT]   = 0;
    args[ARG_CARD]   = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[DEVICE <name>] [UNIT <n>] [CARD <type>]",
                   "Print every counter a SANA-II driver keeps for one unit: "
                   "the standard block and the driver's own named records.");
        return RETURN_ERROR;
    }
    if (args[ARG_DEVICE] != 0)
        device = (const char *)args[ARG_DEVICE];
    if (args[ARG_UNIT] != 0)
        unit = (ULONG)*(LONG *)args[ARG_UNIT];
    if (args[ARG_CARD] != 0)
        card = (const char *)args[ARG_CARD];

    port    = CreateMsgPort();
    req     = (struct IOSana2Req *)ami_alloc((ULONG)sizeof(*req));
    special = ami_alloc((ULONG)sizeof(*special));
    global  = ami_alloc((ULONG)sizeof(*global));
    if (port == NULL || req == NULL || special == NULL || global == NULL)
    {
        tool_error("out of memory");
        rc = RETURN_FAIL;
        goto out;
    }

    tags[tag].ti_Tag  = S2_CopyToBuff;
    tags[tag].ti_Data = (ULONG)nds_copy;
    tag++;
    tags[tag].ti_Tag  = S2_CopyFromBuff;
    tags[tag].ti_Data = (ULONG)nds_copy;
    tag++;
    if (card != NULL && *card != '\0')
    {
        tags[tag].ti_Tag  = S2_AnxCardType;
        tags[tag].ti_Data = (ULONG)card;
        tag++;
    }
    tags[tag].ti_Tag  = TAG_DONE;
    tags[tag].ti_Data = 0;

    req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->ios2_Req.io_Message.mn_Length       = (UWORD)sizeof(*req);
    req->ios2_Req.io_Message.mn_ReplyPort    = port;
    req->ios2_BufferManagement               = tags;

    if (ami_sana2_open_device(device, unit, (struct IORequest *)req) != 0)
    {
        tool_error("cannot open %s unit %lu%s%s", (LONG)device, (LONG)unit,
                   (LONG)(card != NULL ? " card " : ""),
                   (LONG)(card != NULL ? card : ""));
        rc = RETURN_ERROR;
        goto out;
    }

    tool_printf("%s unit %lu%s%s\n", (LONG)device, (LONG)unit,
                (LONG)(card != NULL ? " card " : ""),
                (LONG)(card != NULL ? card : ""));

    req->ios2_Req.io_Command = S2_GETGLOBALSTATS;
    req->ios2_StatData       = global;
    if (DoIO((struct IORequest *)req) == 0)
    {
        tool_printf("  packets received  %10lu    packets sent      %10lu\n",
                    (LONG)global->PacketsReceived, (LONG)global->PacketsSent);
        tool_printf("  bad data          %10lu    overruns          %10lu\n",
                    (LONG)global->BadData, (LONG)global->Overruns);
        tool_printf("  unknown types     %10lu    reconfigurations  %10lu\n",
                    (LONG)global->UnknownTypesReceived,
                    (LONG)global->Reconfigurations);
    }
    else
    {
        tool_printf("  S2_GETGLOBALSTATS refused: error %ld wire %lu\n",
                    (LONG)req->ios2_Req.io_Error, (LONG)req->ios2_WireError);
    }

    special->hdr.RecordCountMax = NDS_RECORDS;
    req->ios2_Req.io_Command    = S2_GETSPECIALSTATS;
    req->ios2_StatData          = special;
    if (DoIO((struct IORequest *)req) == 0)
    {
        ULONG n = special->hdr.RecordCountSupplied;
        ULONG i;

        if (n > NDS_RECORDS)
            n = NDS_RECORDS;
        for (i = 0; i < n; i++)
        {
            tool_printf("  %3lu %10lu  %s\n", (LONG)special->rec[i].Type,
                        (LONG)special->rec[i].Count,
                        (LONG)(special->rec[i].String != NULL
                               ? special->rec[i].String : ""));
        }
    }
    else
    {
        tool_printf("  S2_GETSPECIALSTATS refused: error %ld wire %lu\n",
                    (LONG)req->ios2_Req.io_Error, (LONG)req->ios2_WireError);
    }

    CloseDevice((struct IORequest *)req);

out:
    if (global != NULL)
        ami_free(global);
    if (special != NULL)
        ami_free(special);
    if (req != NULL)
        ami_free(req);
    if (port != NULL)
        DeleteMsgPort(port);
    FreeArgs(rda);
    return (int)rc;
}
