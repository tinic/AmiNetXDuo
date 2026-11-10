/*
 * CreateAmiNetXDuoStatusReport, the half that decides what a line says.
 *
 * Every line is `key=value`.  Nothing here calls AmigaOS, so the host test
 * (test/test_statusreport.c) runs the same code the command does.
 *
 * WHAT MAY BE PRINTED IS AN ALLOWLIST, built up key by key.  An interface file
 * is never copied out and scrubbed: each keyword is looked up in sr_if_keys[],
 * a keyword not there is counted and dropped, and a keyword that is there has
 * its value checked against the shape that keyword takes.  Addresses, names
 * and station addresses are a second class that prints only when the caller
 * set SrOut.addresses.  A password, a key or a token cannot appear, because no
 * row admits one.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_STATUSREPORT_TEXT_H
#define AMINETXDUO_STATUSREPORT_TEXT_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_UNAVAILABLE  "unavailable"
#define SR_INVALID      "invalid"

#define SR_KEY_MAX      96      /* a whole key, dots included               */
#define SR_VALUE_MAX    96      /* a value is cut here                      */
#define SR_LINE_MAX     (SR_KEY_MAX + SR_VALUE_MAX + 4)

/* One finished line, "key=value\n", to wherever the report goes. */
typedef VOID (*SrWrite)(APTR user, const char *line);

typedef struct SrOut
{
    SrWrite write;
    APTR    user;
    BOOL    addresses;          /* ADDRESSES: the second class may print    */
    ULONG   lines;              /* written so far                            */
} SrOut;

/*
 * The console and the file, one line to each.  `put` answers 0 when the line
 * went out.  The first file line that does not stops the file and is kept in
 * file_failed; the console still gets every line.
 */
typedef LONG (*SrPut)(APTR handle, const char *line);

typedef struct SrTee
{
    SrPut   put;
    APTR    console;
    APTR    file;               /* NULL: no file                            */
    BOOL    file_failed;
} SrTee;

/* An SrWrite; `user` is the SrTee. */
VOID sr_tee_write(APTR user, const char *line);

/* ------------------------------------------------------------ one line --- */

/* NULL is SR_UNAVAILABLE.  Control characters in `value` become '?'. */
VOID sr_str(SrOut *o, const char *key, const char *value);
VOID sr_ulong(SrOut *o, const char *key, ULONG value);
VOID sr_long(SrOut *o, const char *key, LONG value);
VOID sr_hex(SrOut *o, const char *key, ULONG value);           /* $0000002a */
VOID sr_yesno(SrOut *o, const char *key, BOOL value);
VOID sr_version(SrOut *o, const char *key, ULONG version, ULONG revision);

/* The second class.  Nothing at all is printed without SrOut.addresses. */
VOID sr_addr_str(SrOut *o, const char *key, const char *value);
VOID sr_addr_ipv4(SrOut *o, const char *key, ULONG addr);      /* host order */
VOID sr_addr_mac(SrOut *o, const char *key, const UBYTE *mac);  /* six bytes */
/* Up to 32 bytes as one run of hex digits, "3604c0a8...".  They can hold an
   address.  NULL bytes is SR_UNAVAILABLE. */
VOID sr_addr_bytes(SrOut *o, const char *key, const UBYTE *bytes, ULONG n);

/* ------------------------------------------------------------- keys --- */

/*
 * "a.b.c" into `dst`, any part NULL to stop early.  A part that is not ours --
 * an interface file's name, a device name -- goes through sr_key_part(), so
 * nothing in it can be read as a second key or a second line.
 */
VOID sr_key(char *dst, ULONG dstlen, const char *a, const char *b,
            const char *c);
VOID sr_key_part(char *dst, ULONG dstlen, const char *text);

/* The value of ", <name> <value>" in a library IdString, up to the next ','
   or ')'.  FALSE when it is not there or is empty. */
BOOL sr_id_field(const char *id, const char *name, char *dst, ULONG dstlen);
VOID sr_key_index(char *dst, ULONG dstlen, const char *prefix, ULONG index,
                  const char *suffix);

/* Appending to a NUL-terminated value, cut at dstlen.  $hex is eight digits. */
VOID sr_cat(char *dst, ULONG dstlen, const char *text);
VOID sr_cat_ulong(char *dst, ULONG dstlen, ULONG value);
VOID sr_cat_long(char *dst, ULONG dstlen, LONG value);
VOID sr_cat_hex(char *dst, ULONG dstlen, ULONG value);

/* ------------------------------------------------------------ decoding --- */

/* The processor and the FPU, from ExecBase->AttnFlags.  Never NULL. */
const char *sr_cpu_name(ULONG attn);
const char *sr_fpu_name(ULONG attn);

/* ------------------------------------------------------ interface files --- */

/*
 * One DEVS:NetInterfaces file, already in memory and NUL-terminated.  Parsed
 * IN PLACE with the configuration layer's own tokenizer, so a key this sees is
 * the key the stack sees.  Prints config.<name>.<keyword>=<value> for every
 * allowlisted keyword, then config.<name>.omitted=<count of the rest>.
 *
 * `device` (may be NULL) receives the DEVICE= driver's file name, without a
 * path, when the value was well formed.  Returns the lines printed.
 */
ULONG sr_interface_text(SrOut *o, const char *ifname, char *text,
                        char *device, ULONG devicelen);

/* ------------------------------------------------------ the probe record --- */

/*
 * Whether one step of an anx driver's probe record may print its value.
 *   1  yes
 *   0  only with SrOut.addresses: it carries station-address or serial bytes
 *  -1  a code this build does not know; print SR_UNAVAILABLE for it
 */
LONG sr_anxdiag_value_class(UWORD code);

/* ------------------------------------------------------ tasks, sockets --- */

/*
 * One Exec task as ShowTasks DETAIL prints it, from shelltools' TaskRow
 * (statusreport_tasks.cpp copies it here).  The process arguments are left
 * out: a command line can carry a password, and no row admits one.
 */
typedef struct SrTask
{
    ULONG       address;
    LONG        cli;
    LONG        priority;
    const char *state;          /* shelltools::task_state()                 */
    BOOL        process;
    ULONG       flags;
    ULONG       sig_alloc;
    ULONG       sig_wait;
    ULONG       sig_recvd;
    ULONG       stack_lower;
    ULONG       stack_upper;
    ULONG       stack_pointer;
    LONG        process_stack;
    ULONG       process_flags;
    const char *name;
    const char *command;        /* "" when none                             */
} SrTask;

/* task.<index>.* */
VOID sr_task(SrOut *o, ULONG index, const SrTask *t);

/* Every task, task.count and task.truncated first.  statusreport_tasks.cpp. */
VOID sr_tasks(SrOut *o);

/* ------------------------------------------------------ return addresses --- */

/*
 * Where a waiting task is: the longwords of its saved stack that point into
 * bsdsocket.library or into a loaded program, as module:hunk+$offset.  The
 * offset is from the start of the hunk's contents, which is the address
 * `m68k-amigaos-objdump -d` prints for hunk 0.  A scan, not a frame walk: a
 * stale longword or a pointer to data reads the same as a return address.
 */
#define SR_RANGES_MAX       512     /* hunks, every module together         */
#define SR_HUNKS_MAX        128     /* of one seglist                        */
#define SR_HUNK_BYTES_MAX   0x01000000UL
#define SR_FRAMES_MAX       16      /* printed for one task                  */
#define SR_STACK_BYTES      1024    /* of saved stack looked at              */
#define SR_MODULE_LEN       24      /* a module name in a frame              */

/* The longword at an even `address`; FALSE when it must not be read. */
typedef BOOL (*SrPeek)(APTR user, ULONG address, ULONG *value);

typedef struct SrRange
{
    ULONG       start;          /* the hunk's contents, segment + 4          */
    ULONG       size;
    const char *name;           /* the module's; the caller keeps it         */
    UWORD       hunk;           /* index in its seglist                      */
    UWORD       library;        /* bsdsocket.library's                       */
} SrRange;

typedef struct SrRanges
{
    SrPeek  peek;
    APTR    user;
    ULONG   count;
    ULONG   low;                /* every range lies in [low, high)           */
    ULONG   high;
    SrRange r[SR_RANGES_MAX];
} SrRanges;

/* No ranges, read through `peek`. */
VOID sr_ranges_init(SrRanges *t, SrPeek peek, APTR user);

/*
 * The hunks of one LoadSeg() seglist (a BPTR).  Per segment: the allocation's
 * size in bytes at -4, the BPTR to the next at +0, contents from +4.  The walk
 * ends at the first value that cannot be one, at SR_HUNKS_MAX, or when the
 * table is full.  A seglist already in the table adds nothing.  Returns the
 * ranges added.
 */
ULONG sr_ranges_add(SrRanges *t, const char *name, BOOL library, ULONG seglist);

/* module.count and module.<n>.* for the library's ranges. */
VOID sr_modules(SrOut *o, const SrRanges *t);

/*
 * task.<index>.frames from `bytes` of saved stack, lowest address first, read
 * as big-endian longwords at every even offset.  Printed only when one of the
 * frames is in the library; TRUE when it was.  /lvo<d> marks a frame whose
 * call was `jsr d(a6)`.  Of more than SR_FRAMES_MAX candidates, those a jsr
 * or bsr comes before are printed first and the rest fill what is left, all
 * in stack order.
 */
BOOL sr_task_frames(SrOut *o, ULONG index, const SrRanges *t,
                    const UBYTE *stack, ULONG bytes);

/* The Exec side of the three above, statusreport_frames.c.  sr_code_begin()
   empties the table and adds the resident bsdsocket.library. */
VOID sr_code_begin(VOID);
VOID sr_code_add_process(ULONG task, const char *name);
VOID sr_code_modules(SrOut *o);
VOID sr_code_task_frames(SrOut *o, ULONG index, ULONG task);

/* baton.* and lock.sb.* off the published health mark. */
VOID sr_locks(SrOut *o);

/* One row of NETSTATUS_OPENERS. */
typedef struct SrOpener
{
    ULONG       task;           /* joins task.<n>.address                    */
    const char *name;
    ULONG       sockets;
    ULONG       break_mask;
    ULONG       flags;          /* NETSTATUS_OPENER_*                        */
} SrOpener;

/* opener.<index>.* */
VOID sr_opener(SrOut *o, ULONG index, const SrOpener *op);

/* One row of the table `netstat ALL` prints (tool_sockets()). */
typedef struct SrSocket
{
    BOOL        tcp;
    const char *state;          /* tool_tcp_state_name(); unused for UDP    */
    UWORD       local_port;
    UWORD       peer_port;
    ULONG       peer_address;   /* host order; the second class             */
    ULONG       queued;         /* UDP receive queue                        */
    ULONG       stalled_ms;
    ULONG       retransmits;
    ULONG       rto_ms;
    ULONG       user_timeout_ms;
} SrSocket;

/* socket.<index>.* */
VOID sr_socket(SrOut *o, ULONG index, const SrSocket *s);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_STATUSREPORT_TEXT_H */
