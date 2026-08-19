/*
 * bpf_write()'s ThreadX bracket, on the host.
 *
 * The shipping src/bsdsocket/bpf.c is linked beside this harness.  Its write
 * injector eventually takes nx_ip_protection in netstack_capture.c; entering
 * that mutex from an ordinary Exec Task is invalid, and omitting the mutex
 * lets Offline drain a SANA-II TX slot before its BeginIO().  These stubs make
 * the required enter -> write -> leave order observable without reproducing
 * either scheduler.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/bpf.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;
static LONG          h_enter_result;
static LONG          h_write_result;
static LONG          h_error_code;
static int           h_stage;
static int           h_order_bad;
static int           h_write_calls;
static int           h_leave_calls;
static APTR          h_owner;
static LONG          h_channel;
static APTR          h_buffer;
static LONG          h_length;

static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
    }
}

static void h_reset(void)
{
    h_enter_result = 0;
    h_write_result = 0;
    h_error_code   = 0;
    h_stage        = 0;
    h_order_bad    = 0;
    h_write_calls  = 0;
    h_leave_calls  = 0;
    h_owner        = NULL;
    h_channel      = 0;
    h_buffer       = NULL;
    h_length       = 0;
}

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    (void)base;
    if (h_stage != 0)
        h_order_bad = 1;
    h_stage = 1;
    return h_enter_result;
}

VOID bsd_nx_leave(struct AmiSocketBase *base)
{
    (void)base;
    if (h_stage != 2)
        h_order_bad = 1;
    h_stage = 3;
    h_leave_calls++;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    (void)base;
    h_error_code = code;
    return -1;
}

LONG ami_bpf_write(APTR owner, LONG channel, APTR buffer, LONG len)
{
    if (h_stage != 1)
        h_order_bad = 1;
    h_stage = 2;
    h_write_calls++;
    h_owner   = owner;
    h_channel = channel;
    h_buffer  = buffer;
    h_length  = len;
    return h_write_result;
}

/* The translation unit contains all eight vectors, so its seven table-only
 * calls resolve here even though this test invokes only the write vector. */
LONG ami_bpf_open(APTR owner, LONG channel)
{ (void)owner; (void)channel; return 0; }
LONG ami_bpf_close(APTR owner, LONG channel)
{ (void)owner; (void)channel; return 0; }
LONG ami_bpf_read(APTR owner, LONG channel, APTR buffer, LONG len)
{ (void)owner; (void)channel; (void)buffer; (void)len; return 0; }
LONG ami_bpf_set_notify_mask(APTR owner, LONG channel, ULONG mask)
{ (void)owner; (void)channel; (void)mask; return 0; }
LONG ami_bpf_set_interrupt_mask(APTR owner, LONG channel, ULONG mask)
{ (void)owner; (void)channel; (void)mask; return 0; }
LONG ami_bpf_ioctl(APTR owner, LONG channel, ULONG command, APTR buffer)
{ (void)owner; (void)channel; (void)command; (void)buffer; return 0; }
LONG ami_bpf_data_waiting(APTR owner, LONG channel)
{ (void)owner; (void)channel; return 0; }
VOID ami_bpf_close_owner(APTR owner)
{ (void)owner; }

int main(void)
{
    struct AmiSocketBase base;
    UBYTE                frame[60];
    LONG                 rc;

    memset(&base, 0, sizeof(base));
    memset(frame, 0x5a, sizeof(frame));

    h_reset();
    h_write_result = (LONG)sizeof(frame);
    rc = bsd_bpf_write(4, frame, (LONG)sizeof(frame), &base);
    h_check(rc == (LONG)sizeof(frame), "successful byte count returned");
    h_check(!h_order_bad && h_stage == 3,
            "enter, write and leave occur in that order");
    h_check(h_write_calls == 1 && h_leave_calls == 1,
            "one write and one leave");
    h_check(h_owner == (APTR)&base && h_channel == 4 && h_buffer == frame &&
                h_length == (LONG)sizeof(frame),
            "write arguments preserved");

    h_reset();
    h_enter_result = -1;
    rc = bsd_bpf_write(2, frame, (LONG)sizeof(frame), &base);
    h_check(rc == -1 && h_error_code == AMI_ENETDOWN,
            "failed adoption reports ENETDOWN");
    h_check(h_write_calls == 0 && h_leave_calls == 0,
            "failed adoption neither writes nor leaves");

    h_reset();
    h_write_result = AMI_BPF_ENOBUFS;
    rc = bsd_bpf_write(1, frame, (LONG)sizeof(frame), &base);
    h_check(rc == -1 && h_error_code == AMI_ENOBUFS,
            "injector error is mapped after leaving");
    h_check(!h_order_bad && h_stage == 3 && h_leave_calls == 1,
            "injector failure still leaves the bracket");

    printf("bpf write serialization: %lu checks, %lu failures\n",
           h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
