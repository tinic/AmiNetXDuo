/* Execute the production reader entry, including its raw Wait boundary.
 * Reuse the delivery harness's device/NetX doubles, then interpose the few
 * boundaries needed to schedule a completion or stop deterministically.
 * The entry carries its pointer in ULONG, so this target must be ILP32.
 * This tests control flow, not real Exec/ThreadX scheduling or detach races.
 * SPDX-License-Identifier: MIT */
#define main sana2_delivery_test_main
#define BeginIO delivery_BeginIO
#define AbortIO delivery_AbortIO
#define FindTask delivery_FindTask
#define AllocSignal delivery_AllocSignal
#define FreeSignal delivery_FreeSignal
#define Wait delivery_Wait
#define ami_sana2_reader_wait_enter delivery_reader_wait_enter
#define ami_sana2_block_leave delivery_block_leave
#define _tx_mutex_get delivery_mutex_get
#define _txe_mutex_get delivery_mutex_get_checked
#define _txe_semaphore_put delivery_semaphore_put
#define _tx_thread_sleep delivery_thread_sleep
#include "test_sana2_rx_host.c"
#undef main
#undef BeginIO
#undef AbortIO
#undef FindTask
#undef AllocSignal
#undef FreeSignal
#undef Wait
#undef ami_sana2_reader_wait_enter
#undef ami_sana2_block_leave
#undef _tx_mutex_get
#undef _txe_mutex_get
#undef _txe_semaphore_put
#undef _tx_thread_sleep

_Static_assert(sizeof(void *) == sizeof(ULONG), "reader argument needs ILP32");

enum BodyMode { BODY_ASSIST, BODY_QUEUED, BODY_STOP, BODY_NO_PORT };
static struct
{
    enum BodyMode mode;
    AmiSana2Reader *reader;
    struct Task task;
    TX_THREAD caller;
    BOOL held, assisting;
    unsigned signals, frees, waits, releases, acquires, ready, exited, reads, polls;
} body;

struct Task *FindTask(STRPTR name)
{
    (VOID)name;
    return &body.task;
}

BYTE AllocSignal(LONG number)
{
    (VOID)number;
    body.signals++;
    if (body.mode == BODY_NO_PORT)
        return -1;
    h_check(body.signals <= 2, "reader allocates only its receive and reap signals");
    return (BYTE)(19 + body.signals);
}

VOID FreeSignal(LONG number)
{
    h_check(number == 20 || number == 21, "reader frees a signal it allocated");
    body.frees++;
    delivery_FreeSignal(number);
}

VOID BeginIO(struct IORequest *req)
{
    delivery_BeginIO(req);
    if (req->io_Command == CMD_READ)
    {
        body.reads++;
        h_check(body.assisting || !body.reader->parked,
                "ordinary reader posting happens outside the parked interval");
        if (body.mode == BODY_QUEUED && body.reads == 1)
        {
            req->io_Error = IOERR_ABORTED;
            AddTail(&req->io_Message.mn_ReplyPort->mp_MsgList, &req->io_Message.mn_Node);
        }
    }
    else if (req->io_Command == ANXD_CMD_RX_POLL && !body.assisting)
    {
        body.polls++;
        h_check(body.held && !body.reader->parked,
                "reader clears parked after reacquiring and before device polling");
        /* End the controlled pass; teardown must collect the posted read. */
        body.reader->stop = TRUE;
    }
}

LONG AbortIO(struct IORequest *req)
{
    delivery_AbortIO(req);
    if (req->io_Command == CMD_READ)
    {
        req->io_Error = IOERR_ABORTED;
        AddTail(&req->io_Message.mn_ReplyPort->mp_MsgList, &req->io_Message.mn_Node);
    }
    return 0;
}

VOID ami_sana2_reader_wait_enter(VOID)
{
    body.releases++;
    h_check(body.reader->parked && body.reader->running && body.held,
            "parked is published before the reader releases its baton");
    body.held = FALSE;
}

VOID ami_sana2_block_leave(VOID)
{
    body.acquires++;
    h_check(body.reader->parked && !body.held,
            "parked remains published through the reacquire boundary");
    body.held = TRUE;
}

ULONG Wait(ULONG mask)
{
    struct IORequest *read = (struct IORequest *)&body.reader->iface->rx[0].slot[0].req;
    body.waits++;
    h_check(body.waits == 1 && body.reader->parked && !body.held,
            "actual reader Wait occurs once, parked and without the baton");
    h_check(mask == ((1UL << 20) | (1UL << 21)),
            "reader waits on its receive and transmit-reap signals");
    if (body.mode == BODY_STOP || body.waits > 1)
        body.reader->stop = TRUE;
    else
    {
        TX_THREAD *saved = _tx_thread_current_ptr;
        UWORD took;
        read->io_Error = IOERR_ABORTED;
        AddTail(&body.reader->port->mp_MsgList, &read->io_Message.mn_Node);
        body.assisting = TRUE;
        _tx_thread_current_ptr = &body.caller;
        h_lock_depth = 1; /* public assist owns the outer IP lock */
        took = ami_sana2_rx_assist_reader(body.reader, 1);
        h_check(took == 1 && h_lock_depth == 1 && body.reader->parked,
                "caller assistance consumes and reposts a completion while reader is parked");
        h_lock_depth = 0;
        _tx_thread_current_ptr = saved;
        body.assisting = FALSE;
    }
    return mask;
}

UINT _tx_mutex_get(TX_MUTEX *mutex, ULONG wait)
{
    if (!body.assisting)
        h_check(!body.reader->parked && body.held,
                "normal drain starts only after parked is cleared and baton restored");
    return delivery_mutex_get(mutex, wait);
}

UINT _txe_mutex_get(TX_MUTEX *mutex, ULONG wait)
{
    return _tx_mutex_get(mutex, wait);
}

UINT _txe_semaphore_put(TX_SEMAPHORE *sem)
{
    if (sem == &body.reader->ready)
    {
        body.ready++;
        h_check(!body.reader->parked && (body.reader->running || body.reader->failed),
                "reader publishes ready after startup or an explicit startup failure");
    }
    if (sem == &body.reader->exited)
    {
        body.exited++;
        h_check(!body.reader->parked && !body.reader->running && body.reader->port == NULL,
                "reader publishes exit after clearing state and closing its port");
    }
    return delivery_semaphore_put(sem);
}

UINT _tx_thread_sleep(ULONG ticks)
{
    h_check(0, "controlled reader needs no pool retry or teardown sleep");
    body.reader->stop = TRUE;
    return delivery_thread_sleep(ticks);
}

/* The unchanged production entry is private. Including its translation unit
 * here lets this fixture call it without exporting a test-only shipping API. */
#include "../../../src/sana2/sana2_rx.c"

static NX_PACKET body_packet;
static UBYTE body_buffer[128];

static void test_reader_body(enum BodyMode mode)
{
    AmiSana2Reader *rd = &iface.reader;
    AmiSana2Rx *rx = &iface.rx[0];
    fixture_init();
    memset(&body, 0, sizeof(body));
    memset(&body_packet, 0, sizeof(body_packet));
    body.mode = mode;
    body.reader = rd;
    body.held = TRUE;
    rd->iface = &iface;
    iface.online = TRUE;
    iface.rx_poll_ok = TRUE;
    iface.rx_dst_off = AMI_ETH_HEADER_SIZE;
    rx->iface = &iface;
    rx->reader = rd;
    rx->depth = 1;
    rx->slot = &slot;
    rx->packet_type = 0x0800;
    slot.packet = &body_packet;
    body_packet.nx_packet_data_start = body_buffer;
    body_packet.nx_packet_data_end = body_buffer + sizeof(body_buffer);
    h_getmsg_real = TRUE;
    h_poll_async = h_poll_reply_now = h_poll_reply_on_abort = FALSE;
    h_poll_error = 0;
    h_poll_begins = h_wait_ios = h_aborts = h_sleeps = h_begins = 0;
    ami_sana2_rx_thread((ULONG)rd);
    h_check(body.ready == 1 && body.exited == 1 && !rd->running && !rd->parked,
            "real reader entry returns with balanced startup and exit notifications");
    h_check(h_wait_ios == 0 && h_sleeps == 0 && h_lock_depth == 0,
            "reader and assistant finish without hidden I/O waits or leaked IP locks");
    if (mode == BODY_NO_PORT)
        h_check(rd->failed && body.frees == 0 && body.waits == 0,
                "port allocation failure exits before publishing parked or freeing a foreign signal");
    else
    {
        h_check(body.frees == 2 && rd->port == NULL && rd->orphans == 0 && slot.packet == NULL,
                "real teardown returns requests and releases both signals and the packet");
        if (mode == BODY_QUEUED)
            h_check(body.waits == 0 && body.releases == 0 && body.acquires == 0,
                    "already queued completion bypasses the parked interval entirely");
        else
            h_check(body.waits == 1 && body.releases == 1 && body.acquires == 1,
                    "completion and stop wakes each balance one release and reacquire");
        if (mode == BODY_ASSIST)
            h_check(body.reads == 2 && body.polls == 1 && h_poll_begins == 2,
                    "assisted wake rearms once then returns to the ordinary reader pass");
        if (mode == BODY_STOP)
            h_check(body.polls == 0 && h_aborts == 1,
                    "stop wake performs no further poll and collects its outstanding read");
    }
}

int main(void)
{
    puts("sana2: real reader parked lifecycle (32-bit host, scripted scheduling boundaries)");
    test_reader_body(BODY_ASSIST);
    test_reader_body(BODY_QUEUED);
    test_reader_body(BODY_STOP);
    test_reader_body(BODY_NO_PORT);
    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures != 0;
}
