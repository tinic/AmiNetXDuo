/* Private stdio hand-off between the Amiga scp launcher and ssh client. */
#ifndef AMIGA_DROPBEAR_STDIO_H
#define AMIGA_DROPBEAR_STDIO_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#define AMIGA_STDIO_MAGIC     0x414e5853UL /* "ANXS" */
#define AMIGA_STDIO_TASK_NAME "AmiNetXDuo scp ssh"
#define AMIGA_STDIO_USERDATA_TAG 1UL
#define AMIGA_MEMPIPE_SIZE       32768UL
#define AMIGA_MEMPIPE_MASK       (AMIGA_MEMPIPE_SIZE - 1UL)

struct amiga_mempipe
{
    volatile ULONG read_pos;
    volatile ULONG write_pos;
    volatile UBYTE reader_closed;
    volatile UBYTE writer_closed;
    struct Task   *reader_task;
    struct Task   *writer_task;
    ULONG          reader_signal;
    ULONG          writer_signal;
    UBYTE          data[AMIGA_MEMPIPE_SIZE];
};

struct amiga_stdio_override
{
    ULONG magic;
    struct amiga_mempipe *input;
    struct amiga_mempipe *output;
    BPTR  error;
    BPTR  command_segment;
    STRPTR arguments;
    ULONG arguments_length;
    volatile LONG *exit_rc;
    volatile int  *exit_done;
    struct Task   *exit_task;
    ULONG exit_signal;
};

static inline int amiga_mempipe_wait(ULONG signal)
{
    ULONG got = Wait(signal | SIGBREAKF_CTRL_C);

    if ((got & SIGBREAKF_CTRL_C) == 0)
        return 0;

    /* Leave the break visible to the owner's cleanup path.  It will forward
       it to the hosted ssh Process before releasing the transport. */
    SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
    return -1;
}

static inline LONG amiga_mempipe_read(struct amiga_mempipe *pipe,
                                      APTR buffer, ULONG length)
{
    UBYTE *out = (UBYTE *)buffer;

    if (length == 0)
        return 0;

    for (;;)
    {
        ULONG used;
        ULONG take;
        ULONG first;

        Forbid();
        used = pipe->write_pos - pipe->read_pos;
        if (used != 0)
        {
            take = used < length ? used : length;
            first = AMIGA_MEMPIPE_SIZE - (pipe->read_pos & AMIGA_MEMPIPE_MASK);
            if (first > take)
                first = take;
            CopyMem(pipe->data + (pipe->read_pos & AMIGA_MEMPIPE_MASK),
                    out, first);
            if (take != first)
                CopyMem(pipe->data, out + first, take - first);
            pipe->read_pos += take;
            if (pipe->writer_task != NULL)
                Signal(pipe->writer_task, pipe->writer_signal);
            Permit();
            return (LONG)take;
        }
        if (pipe->writer_closed)
        {
            Permit();
            return 0;
        }
        Permit();
        if (amiga_mempipe_wait(pipe->reader_signal) != 0)
            return -1;
    }
}

static inline LONG amiga_mempipe_write(struct amiga_mempipe *pipe,
                                       const void *buffer, ULONG length)
{
    const UBYTE *in = (const UBYTE *)buffer;

    if (length == 0)
        return 0;

    for (;;)
    {
        ULONG used;
        ULONG room;
        ULONG put;
        ULONG first;

        Forbid();
        if (pipe->reader_closed)
        {
            Permit();
            return -1;
        }
        used = pipe->write_pos - pipe->read_pos;
        room = AMIGA_MEMPIPE_SIZE - used;
        if (room != 0)
        {
            put = room < length ? room : length;
            first = AMIGA_MEMPIPE_SIZE - (pipe->write_pos & AMIGA_MEMPIPE_MASK);
            if (first > put)
                first = put;
            CopyMem(in, pipe->data + (pipe->write_pos & AMIGA_MEMPIPE_MASK),
                    first);
            if (put != first)
                CopyMem(in + first, pipe->data, put - first);
            pipe->write_pos += put;
            if (pipe->reader_task != NULL)
                Signal(pipe->reader_task, pipe->reader_signal);
            Permit();
            return (LONG)put;
        }
        Permit();
        if (amiga_mempipe_wait(pipe->writer_signal) != 0)
            return -1;
    }
}

static inline int amiga_mempipe_read_ready(const struct amiga_mempipe *pipe)
{
    return pipe->write_pos != pipe->read_pos || pipe->writer_closed;
}

static inline void amiga_mempipe_close_reader(struct amiga_mempipe *pipe)
{
    Forbid();
    pipe->reader_closed = 1;
    if (pipe->writer_task != NULL)
        Signal(pipe->writer_task, pipe->writer_signal);
    Permit();
}

static inline void amiga_mempipe_close_writer(struct amiga_mempipe *pipe)
{
    Forbid();
    pipe->writer_closed = 1;
    if (pipe->reader_task != NULL)
        Signal(pipe->reader_task, pipe->reader_signal);
    Permit();
}

#endif
