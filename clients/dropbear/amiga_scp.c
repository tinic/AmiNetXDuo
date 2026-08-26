/* AmigaOS process and filesystem support for Dropbear scp.
 *
 * Dropbear's scp is the traditional SCP protocol program.  On Unix it forks
 * dbclient and joins the two programs with anonymous pipes.  AmigaOS has no
 * fork(), but it has exactly the other two primitives required here: shared
 * memory and independent Processes.  The ssh executable is loaded and
 * started directly with separate input, output and error handles, so the SCP
 * byte stream cannot be polluted by client diagnostics.
 *
 * build.sh renames upstream do_cmd() in a checked build-directory source copy;
 * scpmisc.o's colon() is made weak.  These strong definitions replace both
 * without modifying third_party/dropbear.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "scpmisc.h"
#include "amiga_stdio.h"

#define SCP_FD_BASE       240
#define SCP_FD_COUNT      4
#define SCP_DIR_FD_BASE   208
#define SCP_DIR_FD_COUNT  (SCP_FD_BASE - SCP_DIR_FD_BASE)
#define SCP_COMMAND_MAX   2048
#define SCP_RUNNER        "PROGDIR:scp-runner"
#define SCP_CHILD_PID     ((pid_t)2001)
#define AMIGA_EPOCH       (2922L * 86400L)

extern arglist args;
extern char   *ssh_program;
extern pid_t   do_cmd_pid;

/* newlib's open/read/write/close object publishes the descriptor table.  The
 * names are private to that pinned libc, but using the table here is better
 * than claiming ftruncate succeeded while leaving an old tail on a file. */
extern BPTR *__fh;
extern long  __maxfh;

extern int __real_open(const char *path, int flags, ...);
extern int __real_read(int fd, void *buf, size_t len);
extern int __real_write(int fd, const void *buf, size_t len);
extern int __real_close(int fd);
extern int __real_fstat(int fd, struct stat *st);

struct scp_fd
{
    struct amiga_mempipe *pipe;
    unsigned readable : 1;
};

static struct scp_fd scp_fds[SCP_FD_COUNT];

struct scp_dir_fd
{
    int         used;
    struct stat st;
};

static struct scp_dir_fd scp_dir_fds[SCP_DIR_FD_COUNT];

struct scp_child
{
    volatile int     active;
    volatile int     done;
    LONG             rc;
    struct Task     *parent;
    struct Process  *process;
    LONG             signal_bit;
    BPTR             child_in;
    BPTR             child_out;
    BPTR             error_dest;
    struct amiga_mempipe input_pipe;
    struct amiga_mempipe output_pipe;
    struct amiga_stdio_override stdio;
    char             arguments[SCP_COMMAND_MAX];
};

static struct scp_child scp_child;
static BPTR scp_error_stream(void);

unsigned long amiga_client_stack_size(void)
{
    return 128UL * 1024UL;
}

/* Upstream uses exit() for protocol and argument errors.  Return those through
   the argv shim as command status as well, so one failed copy cannot terminate
   the AmigaDOS process which is hosting the command. */
int amiga_client_exit_returns(void)
{
    return 1;
}

/* Is this exact pointer still one of Exec's tasks?  Disable(), rather than
   Forbid(), is required because an interrupt can move a task between the
   scheduler lists.  The pointer is compared only and never dereferenced. */
static int scp_task_on_list(struct List *list, struct Task *task)
{
    struct Node *node;

    for (node = list->lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
    {
        if ((struct Task *)node == task)
            return 1;
    }
    return 0;
}

static int scp_task_alive(struct Task *task)
{
    int alive;

    if (task == NULL)
        return 0;
    Disable();
    alive = (SysBase->ThisTask == task
             || scp_task_on_list(&SysBase->TaskReady, task)
             || scp_task_on_list(&SysBase->TaskWait, task));
    Enable();
    return alive;
}

/* SCP uses strtod() only for its -l Kbit/s rate.  Newlib's general parser
   opens mathieeedoubtrans.library and mathieeesingbas.library even when -l is
   absent.  Keep this deliberately small decimal grammar and the client keeps
   the same sole runtime dependency as ssh: mathieeedoubbas.library. */
double amiga_scp_strtod(const char *text, char **endptr)
{
    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *expmark;
    double value = 0.0;
    double place = 0.1;
    int negative = 0;
    int digits = 0;
    int exponent = 0;
    int exp_negative = 0;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\f' || *p == '\v')
        p++;
    if (*p == '+' || *p == '-')
    {
        negative = (*p == '-');
        p++;
    }
    while (*p >= '0' && *p <= '9')
    {
        value = value * 10.0 + (double)(*p++ - '0');
        digits = 1;
    }
    if (*p == '.')
    {
        p++;
        while (*p >= '0' && *p <= '9')
        {
            value += (double)(*p++ - '0') * place;
            place *= 0.1;
            digits = 1;
        }
    }
    if (!digits)
    {
        if (endptr != NULL)
            *endptr = (char *)text;
        return 0.0;
    }

    expmark = p;
    if (*p == 'e' || *p == 'E')
    {
        const unsigned char *first;

        p++;
        if (*p == '+' || *p == '-')
        {
            exp_negative = (*p == '-');
            p++;
        }
        first = p;
        while (*p >= '0' && *p <= '9')
        {
            if (exponent < 400)
                exponent = exponent * 10 + (*p - '0');
            p++;
        }
        if (p == first)
            p = expmark;
    }
    while (exponent-- > 0)
        value = exp_negative ? value / 10.0 : value * 10.0;

    if (endptr != NULL)
        *endptr = (char *)p;
    return negative ? -value : value;
}

__attribute__((constructor)) static void scp_no_requesters(void)
{
    struct Process *p = (struct Process *)FindTask(NULL);

    if (p != NULL && p->pr_Task.tc_Node.ln_Type == NT_PROCESS)
        p->pr_WindowPtr = (APTR)-1;
}

static int is_scp_fd(int fd)
{
    return fd >= SCP_FD_BASE && fd < SCP_FD_BASE + SCP_FD_COUNT;
}

static int scp_fd_take(struct amiga_mempipe *pipe, int readable)
{
    int i;

    for (i = 0; i < SCP_FD_COUNT; i++)
    {
        if (scp_fds[i].pipe == NULL)
        {
            scp_fds[i].pipe = pipe;
            scp_fds[i].readable = readable ? 1U : 0U;
            return SCP_FD_BASE + i;
        }
    }

    errno = EMFILE;
    return -1;
}

int __wrap_open(const char *path, int flags, ...)
{
    va_list ap;
    int     mode = 0;
    int     fd;
    int     i;
    struct stat st;

    if (path != NULL && strcmp(path, "/dev/null") == 0)
        path = "NIL:";

    va_start(ap, flags);
    if ((flags & O_CREAT) != 0)
        mode = va_arg(ap, int);
    va_end(ap);
    fd = __real_open(amiga_fix_path(path), flags, mode);
    if (fd >= 0 || (flags & O_ACCMODE) != O_RDONLY
        || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return fd;

    /* Unix permits opening a directory and SCP relies on fstat() of that fd
       before it calls opendir().  AmigaDOS has only Lock() for directories,
       so publish the already-resolved metadata through a pseudo descriptor. */
    for (i = 0; i < SCP_DIR_FD_COUNT; i++)
    {
        if (!scp_dir_fds[i].used)
        {
            scp_dir_fds[i].used = 1;
            scp_dir_fds[i].st = st;
            return SCP_DIR_FD_BASE + i;
        }
    }
    errno = EMFILE;
    return -1;
}

int __wrap_fstat(int fd, struct stat *st)
{
    if (fd >= SCP_DIR_FD_BASE && fd < SCP_FD_BASE)
    {
        struct scp_dir_fd *dir = &scp_dir_fds[fd - SCP_DIR_FD_BASE];

        if (!dir->used || st == NULL)
        {
            errno = EBADF;
            return -1;
        }
        *st = dir->st;
        return 0;
    }
    return __real_fstat(fd, st);
}

int __wrap_read(int fd, void *buf, size_t len)
{
    if (is_scp_fd(fd))
    {
        struct scp_fd *s = &scp_fds[fd - SCP_FD_BASE];
        LONG n;

        if (s->pipe == NULL || !s->readable)
        {
            errno = EBADF;
            return -1;
        }
        n = amiga_mempipe_read(s->pipe, (APTR)buf, (ULONG)len);
        if (n < 0)
        {
            errno = EIO;
            return -1;
        }
        return (int)n;
    }
    return __real_read(fd, buf, len);
}

int __wrap_write(int fd, const void *buf, size_t len)
{
    if (is_scp_fd(fd))
    {
        struct scp_fd *s = &scp_fds[fd - SCP_FD_BASE];
        LONG n;

        if (s->pipe == NULL || s->readable)
        {
            errno = EBADF;
            return -1;
        }
        n = amiga_mempipe_write(s->pipe, buf, (ULONG)len);
        if (n < 0)
        {
            errno = EIO;
            return -1;
        }
        return (int)n;
    }
    return __real_write(fd, buf, len);
}

int __wrap_close(int fd)
{
    if (fd >= SCP_DIR_FD_BASE && fd < SCP_FD_BASE)
    {
        struct scp_dir_fd *dir = &scp_dir_fds[fd - SCP_DIR_FD_BASE];

        if (!dir->used)
        {
            errno = EBADF;
            return -1;
        }
        memset(dir, 0, sizeof(*dir));
        return 0;
    }

    if (is_scp_fd(fd))
    {
        struct scp_fd *s = &scp_fds[fd - SCP_FD_BASE];

        if (s->pipe == NULL)
        {
            errno = EBADF;
            return -1;
        }
        if (s->readable)
            amiga_mempipe_close_reader(s->pipe);
        else
            amiga_mempipe_close_writer(s->pipe);
        memset(s, 0, sizeof(*s));
        return 0;
    }

    /* The Shell owns these three handles and closes them after we return. */
    if (fd >= 0 && fd <= 2)
        return 0;
    return __real_close(fd);
}

int __wrap_ftruncate(int fd, off_t length)
{
    BPTR h;

    if (fd < 0 || fd >= __maxfh || __fh == NULL || length < 0
        || length > (off_t)LONG_MAX || (h = __fh[fd]) == (BPTR)0)
    {
        errno = EINVAL;
        return -1;
    }

    if (SetFileSize(h, (LONG)length, OFFSET_BEGINNING) < 0)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

static BPTR scp_error_stream(void)
{
    struct Process *p = (struct Process *)FindTask(NULL);

    if (p != NULL && p->pr_Task.tc_Node.ln_Type == NT_PROCESS
        && p->pr_CES != (BPTR)0)
        return p->pr_CES;
    return Output();
}

static int append_shell_arg(char *line, size_t cap, size_t *used,
                            const char *arg)
{
    const unsigned char *p = (const unsigned char *)arg;

    if (*used + 2 >= cap)
        return -1;
    if (*used != 0)
        line[(*used)++] = ' ';
    line[(*used)++] = '"';

    while (*p != '\0')
    {
        if (*p == '\r' || *p == '\n')
            return -1;
        if (*p == '*' || *p == '"' || *p == '$')
        {
            if (*used + 1 >= cap)
                return -1;
            line[(*used)++] = '*';
        }
        if (*used + 1 >= cap)
            return -1;
        line[(*used)++] = (char)*p++;
    }

    if (*used + 1 >= cap)
        return -1;
    line[(*used)++] = '"';
    line[*used] = '\0';
    return 0;
}

static int scp_start_ssh(struct scp_child *c)
{
    BPTR seg;
    BPTR runner_seg;
    BPTR parent_in;
    BPTR parent_out;
    struct Process *p;

    seg = LoadSeg((CONST_STRPTR)ssh_program);
    if (seg == (BPTR)0)
    {
        errno = ENOENT;
        return -1;
    }
    runner_seg = LoadSeg((CONST_STRPTR)SCP_RUNNER);
    if (runner_seg == (BPTR)0)
    {
        UnLoadSeg(seg);
        errno = ENOENT;
        return -1;
    }
    parent_in = c->child_in;
    parent_out = c->child_out;
    c->stdio.magic           = AMIGA_STDIO_MAGIC;
    c->stdio.input           = &c->input_pipe;
    c->stdio.output          = &c->output_pipe;
    c->stdio.error           = c->error_dest;
    c->stdio.command_segment = seg;
    c->stdio.arguments       = (STRPTR)c->arguments;
    c->stdio.arguments_length = (ULONG)strlen(c->arguments);
    c->stdio.exit_rc         = &c->rc;
    c->stdio.exit_done       = &c->done;
    c->stdio.exit_task       = c->parent;
    c->stdio.exit_signal     = 1UL << c->signal_bit;

    Forbid();
    p = CreateNewProcTags(NP_Seglist,     (ULONG)runner_seg,
                          NP_FreeSeglist, (ULONG)TRUE,
                          NP_Cli,         (ULONG)TRUE,
                          NP_Name,        (ULONG)AMIGA_STDIO_TASK_NAME,
                          NP_CommandName, (ULONG)ssh_program,
                          NP_StackSize,   (ULONG)(32UL * 1024UL),
                          NP_Input,       (ULONG)parent_in,
                          NP_Output,      (ULONG)parent_out,
                          NP_Error,       (ULONG)c->error_dest,
                          NP_CloseInput,  (ULONG)FALSE,
                          NP_CloseOutput, (ULONG)FALSE,
                          NP_CloseError,  (ULONG)FALSE,
                          TAG_DONE);
    if (p != NULL)
    {
        ULONG parent_signal = 1UL << c->signal_bit;

        c->input_pipe.reader_task   = &p->pr_Task;
        c->input_pipe.reader_signal = SIGBREAKF_CTRL_F;
        c->input_pipe.writer_task   = c->parent;
        c->input_pipe.writer_signal = parent_signal;
        c->output_pipe.reader_task   = c->parent;
        c->output_pipe.reader_signal = parent_signal;
        c->output_pipe.writer_task   = &p->pr_Task;
        c->output_pipe.writer_signal = SIGBREAKF_CTRL_F;

        p->pr_Task.tc_UserData = (APTR)((ULONG)&c->stdio
                                      | AMIGA_STDIO_USERDATA_TAG);
        c->process = p;
        c->active = 1;
    }
    Permit();

    if (p == NULL)
    {
        UnLoadSeg(seg);
        UnLoadSeg(runner_seg);
        c->stdio.command_segment = (BPTR)0;
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

static void scp_child_wait(void)
{
    struct Task *target;

    if (!scp_child.active)
        return;
    target = scp_child.process != NULL ? &scp_child.process->pr_Task : NULL;
    while (!scp_child.done)
    {
        /* Poll by one system tick instead of waiting only on the runner's
           signal: if RunCommand or the runner faults before it can post that
           signal, an unbounded Wait() would leave the Shell hung. */
        if (!scp_task_alive(target))
        {
            scp_child.rc = RETURN_FAIL;
            scp_child.done = 1;
            break;
        }
        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0)
        {
            SetSignal(0, SIGBREAKF_CTRL_C);
            Signal(target, SIGBREAKF_CTRL_C);
        }
        Delay(1);
    }

    /* The external runner signals after RunCommand returned.  Wait for its
       exact Exec task so a following SCP cannot race runner-segment cleanup. */
    while (scp_task_alive(target))
        Delay(1);

    Delay(1);

    /* RunCommand() has returned, and the separate runner has now completed
       dos.library's Process-deactivation tail.  Only at this point is it safe
       to release the dbclient SegList. */
    if (scp_child.stdio.command_segment != (BPTR)0)
    {
        UnLoadSeg(scp_child.stdio.command_segment);
        scp_child.stdio.command_segment = (BPTR)0;
        Delay(1);
    }

    if (scp_child.child_in != (BPTR)0)
    {
        Close(scp_child.child_in);
        scp_child.child_in = (BPTR)0;
    }
    if (scp_child.child_out != (BPTR)0)
    {
        Close(scp_child.child_out);
        scp_child.child_out = (BPTR)0;
    }

}

static void scp_cleanup(void)
{
    int i;

    for (i = 0; i < SCP_FD_COUNT; i++)
    {
        if (scp_fds[i].pipe != NULL)
        {
            if (scp_fds[i].readable)
                amiga_mempipe_close_reader(scp_fds[i].pipe);
            else
                amiga_mempipe_close_writer(scp_fds[i].pipe);
            memset(&scp_fds[i], 0, sizeof(scp_fds[i]));
        }
    }
    scp_child_wait();
}

int do_cmd(char *host, char *remuser, char *remote_cmd,
           int *fdin, int *fdout)
{
    size_t used = 0;
    u_int i;
    int  readfd = -1;
    int  writefd = -1;

    if (host == NULL || remote_cmd == NULL || fdin == NULL || fdout == NULL
        || scp_child.active)
    {
        errno = EINVAL;
        return -1;
    }
    memset(&scp_child, 0, sizeof(scp_child));
    scp_child.signal_bit = AllocSignal(-1);
    if (scp_child.signal_bit < 0)
    {
        errno = EAGAIN;
        return -1;
    }
    scp_child.parent = FindTask(NULL);
    scp_child.error_dest = scp_error_stream();

    for (i = 1; i < args.num; i++)
    {
        if (append_shell_arg(scp_child.arguments,
                             sizeof(scp_child.arguments),
                             &used, args.list[i]) != 0)
            goto toolong;
    }
    if (remuser != NULL)
    {
        char user[260];

        if (snprintf(user, sizeof(user), "-l%s", remuser) >= (int)sizeof(user)
            || append_shell_arg(scp_child.arguments,
                                sizeof(scp_child.arguments),
                                &used, user) != 0)
            goto toolong;
    }
    if (append_shell_arg(scp_child.arguments, sizeof(scp_child.arguments), &used,
                         host) != 0
        || append_shell_arg(scp_child.arguments, sizeof(scp_child.arguments),
                            &used,
                            remote_cmd) != 0)
        goto toolong;
    if (used + 2 > sizeof(scp_child.arguments))
        goto toolong;
    scp_child.arguments[used++] = '\n';
    scp_child.arguments[used] = '\0';

    /* RunCommand() temporarily rewrites its Process input handle to expose the
       argument string.  It must not touch the invoking Shell's live handle.
       These private placeholders remain owned here: NP_Close*=FALSE, and the
       parent closes each exactly once after the runner has disappeared. */
    scp_child.child_in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    scp_child.child_out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (scp_child.child_in == (BPTR)0 || scp_child.child_out == (BPTR)0)
        goto fail;

    writefd = scp_fd_take(&scp_child.input_pipe, 0);
    if (writefd < 0)
        goto fail;
    readfd = scp_fd_take(&scp_child.output_pipe, 1);
    if (readfd < 0)
        goto fail;

    if (scp_start_ssh(&scp_child) != 0)
        goto fail;

    do_cmd_pid = SCP_CHILD_PID;
    *fdin = readfd;
    *fdout = writefd;
    (VOID)atexit(scp_cleanup);
    return 0;

toolong:
    errno = E2BIG;
fail:
    if (readfd >= 0) (VOID)__wrap_close(readfd);
    if (writefd >= 0) (VOID)__wrap_close(writefd);
    if (scp_child.child_in != (BPTR)0) Close(scp_child.child_in);
    if (scp_child.child_out != (BPTR)0) Close(scp_child.child_out);
    if (scp_child.signal_bit >= 0) FreeSignal(scp_child.signal_bit);
    memset(&scp_child, 0, sizeof(scp_child));
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    LONG rc;

    (void)options;
    if (!scp_child.active || (pid != -1 && pid != SCP_CHILD_PID))
    {
        errno = ECHILD;
        return -1;
    }

    scp_child_wait();
    rc = scp_child.rc;
    if (rc < 0)
        rc = 127;
    if (status != NULL)
        *status = (int)((rc & 0xff) << 8);

    FreeSignal(scp_child.signal_bit);
    memset(&scp_child, 0, sizeof(scp_child));
    do_cmd_pid = -1;
    return SCP_CHILD_PID;
}

void (*signal(int sig, void (*handler)(int)))(int)
{
    (void)sig;
    (void)handler;
    return SIG_DFL;
}

int kill(pid_t pid, int sig)
{
    if (pid == SCP_CHILD_PID && scp_child.active && !scp_child.done
        && scp_child.process != NULL)
    {
        Signal(&scp_child.process->pr_Task,
               sig == SIGINT ? SIGBREAKF_CTRL_C : SIGBREAKF_CTRL_D);
        return 0;
    }
    errno = ESRCH;
    return -1;
}

int chmod(const char *path, mode_t mode)
{
    ULONG protection = 0;

    if ((mode & S_IRUSR) == 0) protection |= FIBF_READ;
    if ((mode & S_IWUSR) == 0) protection |= FIBF_WRITE | FIBF_DELETE;
    if ((mode & S_IXUSR) == 0) protection |= FIBF_EXECUTE;
    if (!SetProtection((CONST_STRPTR)amiga_fix_path(path), protection))
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int utimes(const char *path, const struct timeval *times)
{
    struct DateStamp ds;
    time_t sec;

    if (path == NULL || times == NULL)
    {
        errno = EFAULT;
        return -1;
    }
    sec = times[1].tv_sec - (time_t)AMIGA_EPOCH;
    if (sec < 0)
        sec = 0;
    ds.ds_Days = (LONG)(sec / 86400L);
    sec %= 86400L;
    ds.ds_Minute = (LONG)(sec / 60L);
    ds.ds_Tick = (LONG)((sec % 60L) * TICKS_PER_SECOND
                 + times[1].tv_usec / (1000000L / TICKS_PER_SECOND));
    if (!SetFileDate((CONST_STRPTR)amiga_fix_path(path), &ds))
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

struct DIR
{
    BPTR                 lock;
    struct FileInfoBlock fib __attribute__((aligned(4)));
    struct dirent        entry;
};

DIR *opendir(const char *path)
{
    DIR *dir = (DIR *)calloc(1, sizeof(*dir));

    if (dir == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }
    dir->lock = Lock((CONST_STRPTR)amiga_fix_path(path), SHARED_LOCK);
    if (dir->lock == (BPTR)0)
    {
        free(dir);
        errno = ENOENT;
        return NULL;
    }
    if (!Examine(dir->lock, &dir->fib) || dir->fib.fib_DirEntryType <= 0)
    {
        UnLock(dir->lock);
        free(dir);
        errno = ENOTDIR;
        return NULL;
    }
    return dir;
}

struct dirent *readdir(DIR *dir)
{
    if (dir == NULL)
    {
        errno = EBADF;
        return NULL;
    }
    if (!ExNext(dir->lock, &dir->fib))
        return NULL;
    dir->entry.d_ino = 1;
    strncpy(dir->entry.d_name, (const char *)dir->fib.fib_FileName,
            sizeof(dir->entry.d_name) - 1);
    dir->entry.d_name[sizeof(dir->entry.d_name) - 1] = '\0';
    return &dir->entry;
}

int closedir(DIR *dir)
{
    if (dir == NULL)
    {
        errno = EBADF;
        return -1;
    }
    UnLock(dir->lock);
    free(dir);
    return 0;
}

int access(const char *path, int mode)
{
    struct stat st;

    (void)mode;
    return stat(path, &st);
}

mode_t umask(mode_t mask)
{
    static mode_t current;
    mode_t old = current;

    current = mask;
    return old;
}

int fcntl(int fd, int command, ...)
{
    if (command == F_GETFL && fd >= 0 && fd <= 2)
        return 0;
    errno = EBADF;
    return -1;
}

int chdir(const char *path)
{
    BPTR lock;
    BPTR old;

    lock = Lock((CONST_STRPTR)amiga_fix_path(path), SHARED_LOCK);
    if (lock == (BPTR)0)
    {
        errno = ENOENT;
        return -1;
    }
    old = CurrentDir(lock);
    if (old != (BPTR)0)
        UnLock(old);
    return 0;
}

pid_t setsid(void)
{
    errno = ENOSYS;
    return -1;
}

int execvp(const char *file, char *const argv[])
{
    (void)file;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

/* flags is always zero in scp.  Keep the matcher local rather than accepting
 * every name: this check is what prevents a remote server from replying to a
 * wildcard request with an unrelated file. */
static int scp_class_match(const unsigned char **pattern, unsigned char value)
{
    const unsigned char *p = *pattern;
    int negate = 0;
    int matched = 0;

    if (*p == '!' || *p == '^')
    {
        negate = 1;
        p++;
    }
    if (*p == ']')
    {
        matched = (value == ']');
        p++;
    }
    while (*p != '\0' && *p != ']')
    {
        unsigned char first = *p++;
        unsigned char last = first;

        if (first == '\\' && *p != '\0')
            first = last = *p++;
        if (*p == '-' && p[1] != '\0' && p[1] != ']')
        {
            p++;
            last = *p++;
            if (last == '\\' && *p != '\0')
                last = *p++;
        }
        if (value >= first && value <= last)
            matched = 1;
    }
    if (*p != ']')
        return -1;
    *pattern = p + 1;
    return negate ? !matched : matched;
}

static int scp_fnmatch(const unsigned char *pattern, const unsigned char *name)
{
    while (*pattern != '\0')
    {
        unsigned char token = *pattern++;

        if (token == '*')
        {
            while (*pattern == '*') pattern++;
            if (*pattern == '\0') return 1;
            do {
                if (scp_fnmatch(pattern, name)) return 1;
            } while (*name++ != '\0');
            return 0;
        }
        if (*name == '\0')
            return 0;
        if (token == '?')
        {
            name++;
            continue;
        }
        if (token == '[')
        {
            int yes = scp_class_match(&pattern, *name++);

            if (yes <= 0) return 0;
            continue;
        }
        if (token == '\\' && *pattern != '\0')
            token = *pattern++;
        if (token != *name++)
            return 0;
    }
    return *name == '\0';
}

int fnmatch(const char *pattern, const char *name, int flags)
{
    (void)flags;
    return scp_fnmatch((const unsigned char *)pattern,
                       (const unsigned char *)name) ? 0 : 1;
}

/* A colon after a mounted Amiga volume or assign is local, not SCP's
 * host:path separator.  user@host:path and bracketed IPv6 keep the upstream
 * interpretation; /volume/path is naturally local too. */
char *colon(char *path)
{
    char *p;
    int bracket = 0;

    if (path == NULL || path[0] == ':')
        return NULL;
    if (path[0] == '[')
        bracket = 1;

    for (p = path; *p != '\0'; p++)
    {
        if (*p == '@' && p[1] == '[')
            bracket = 1;
        if (*p == ']' && p[1] == ':' && bracket)
            return p + 1;
        if (*p == '/')
            return NULL;
        if (*p == ':' && !bracket)
        {
            if (strchr(path, '@') == NULL)
            {
                char volume[128];
                size_t n = (size_t)(p - path) + 1;

                if (n < sizeof(volume))
                {
                    BPTR lock;

                    memcpy(volume, path, n);
                    volume[n] = '\0';
                    lock = Lock((CONST_STRPTR)volume, SHARED_LOCK);
                    if (lock != (BPTR)0)
                    {
                        UnLock(lock);
                        return NULL;
                    }
                }
            }
            return p;
        }
    }
    return NULL;
}

/* Only the upstream local-to-local and remote-to-remote paths can reach
 * these.  They fail explicitly; normal local<->remote copies use do_cmd(). */
pid_t fork(void)  { errno = ENOSYS; return -1; }
pid_t vfork(void) { errno = ENOSYS; return -1; }
int pipe(int fds[2]) { (void)fds; errno = ENOSYS; return -1; }
int dup2(int from, int to) { (void)from; (void)to; errno = ENOSYS; return -1; }
