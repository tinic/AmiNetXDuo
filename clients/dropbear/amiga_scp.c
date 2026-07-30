/*
 * clients/dropbear/amiga_scp.c -- the POSIX surface scp wants and AmigaOS does
 * not have.
 *
 * Linked into scp only, never into dbclient or dropbear.  Those two get
 * amiga_dropbear.c, which defines several of these names for its own descriptor
 * map; one object per program keeps the two from colliding and keeps scp out of
 * a map it is not part of.
 *
 * scp itself is unmodified upstream (third_party/dropbear/src/scp.c), and in the
 * two modes a server invokes -- `scp -t` to receive and `scp -f` to send -- it
 * is stdin, stdout and the filesystem and nothing else.  So the functions here
 * split cleanly in two:
 *
 *   real          access, chdir, chmod, umask, utimes, fnmatch, opendir,
 *                 readdir, closedir.  All reachable in -t/-f, all implemented
 *                 over dos.library.
 *   refused       fork, vfork, execvp, dup2, setsid, kill, pipe, waitpid,
 *                 fcntl, signal.  Only scp's own CLIENT mode reaches these --
 *                 do_cmd() forks and execs `ssh` across two pipes -- and
 *                 AmigaOS has no fork.  They fail with ENOSYS rather than
 *                 pretend, so `scp file host:path` typed on the Amiga reports
 *                 an error instead of hanging.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <fnmatch.h>

/* clients/compat/amiga_compat.h, force-included by the build. */
extern const char *amiga_fix_path(const char *path);

/* ------------------------------------------------------------- the real --- */

/*
 * access().  A Lock() answers F_OK and R_OK; W_OK is the protection bits, which
 * on AmigaOS are ACTIVE-LOW -- FIBF_WRITE set means write is DENIED.
 */
int access(const char *path, int mode)
{
    BPTR                  lock;
    struct FileInfoBlock *fib;
    int                   ok = 0;

    lock = Lock((CONST_STRPTR)amiga_fix_path(path), ACCESS_READ);
    if (lock == (BPTR)0)
    {
        errno = ENOENT;
        return -1;
    }

    if ((mode & W_OK) == 0)
    {
        UnLock(lock);
        return 0;
    }

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib != NULL)
    {
        if (Examine(lock, fib))
            ok = (fib->fib_Protection & FIBF_WRITE) == 0;
        FreeDosObject(DOS_FIB, fib);
    }

    UnLock(lock);

    if (!ok)
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int chdir(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)amiga_fix_path(path), ACCESS_READ);
    BPTR old;

    if (lock == (BPTR)0)
    {
        errno = ENOENT;
        return -1;
    }

    old = CurrentDir(lock);
    UnLock(old);

    return 0;
}

/*
 * chmod().  The three Unix read/write/execute bits map onto the Amiga's, which
 * are inverted: a set FIBF_ bit denies.  Owner bits only -- AmigaOS has group
 * and other bits but no notion of which user you are.
 */
int chmod(const char *path, mode_t mode)
{
    ULONG prot = 0;

    if ((mode & S_IRUSR) == 0) prot |= FIBF_READ;
    if ((mode & S_IWUSR) == 0) prot |= FIBF_WRITE | FIBF_DELETE;
    if ((mode & S_IXUSR) == 0) prot |= FIBF_EXECUTE;

    if (!SetProtection((CONST_STRPTR)amiga_fix_path(path), prot))
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

/* No umask on AmigaOS.  scp reads it, applies it and puts it back, so a
   constant zero is both honest and harmless. */
mode_t umask(mode_t mask)
{
    (void)mask;
    return 0;
}

/*
 * utimes(), for scp -p.  AmigaOS keeps one date per file, not three, and
 * SetFileDate() sets it, so the modification time is honoured and the access
 * time is dropped.  The epoch is 1978-01-01, not 1970.
 */
#define AMIGA_EPOCH_OFFSET  252460800L      /* 1978-01-01 minus 1970-01-01 */

int utimes(const char *path, const struct timeval tv[2])
{
    struct DateStamp ds;
    long             secs;

    if (tv == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    secs = (long)tv[1].tv_sec - AMIGA_EPOCH_OFFSET;   /* [1] is mtime */
    if (secs < 0)
        secs = 0;

    ds.ds_Days   = secs / (24L * 60L * 60L);
    ds.ds_Minute = (secs % (24L * 60L * 60L)) / 60L;
    ds.ds_Tick   = (secs % 60L) * TICKS_PER_SECOND;

    if (!SetFileDate((CONST_STRPTR)amiga_fix_path(path), &ds))
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

/*
 * fnmatch(), enough of it for the one call scp makes: -r against the patterns
 * a client sent.  '*', '?' and '[...]' with ranges and a leading '!' or '^'
 * negation.  No FNM_PATHNAME, no FNM_PERIOD, no collating classes; flags is
 * accepted and ignored, which is what the one call site passes (0).
 */
static int scp_match(const char *pat, const char *str)
{
    while (*pat != '\0')
    {
        if (*pat == '*')
        {
            pat++;
            if (*pat == '\0')
                return 1;
            while (*str != '\0')
            {
                if (scp_match(pat, str))
                    return 1;
                str++;
            }
            return scp_match(pat, str);
        }

        if (*str == '\0')
            return 0;

        if (*pat == '?')
        {
            pat++;
            str++;
            continue;
        }

        if (*pat == '[')
        {
            int neg = 0;
            int hit = 0;

            pat++;
            if (*pat == '!' || *pat == '^')
            {
                neg = 1;
                pat++;
            }
            while (*pat != '\0' && *pat != ']')
            {
                if (pat[1] == '-' && pat[2] != '\0' && pat[2] != ']')
                {
                    if ((unsigned char)*str >= (unsigned char)pat[0] &&
                        (unsigned char)*str <= (unsigned char)pat[2])
                        hit = 1;
                    pat += 3;
                }
                else
                {
                    if (*pat == *str)
                        hit = 1;
                    pat++;
                }
            }
            if (*pat == ']')
                pat++;
            if (hit == neg)
                return 0;
            str++;
            continue;
        }

        if (*pat != *str)
            return 0;
        pat++;
        str++;
    }

    return *str == '\0';
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    (void)flags;

    if (pattern == NULL || string == NULL)
        return FNM_NOMATCH;

    return scp_match(pattern, string) ? 0 : FNM_NOMATCH;
}

/*
 * opendir/readdir/closedir over Lock()/Examine()/ExNext(), for scp -r.
 *
 * The FileInfoBlock comes from AllocDosObject() rather than the stack or
 * AllocVec(): Examine() needs it longword aligned and that is the documented
 * way to get one that is.
 *
 * Examine() fills the block with the directory itself and ExNext() then walks
 * the entries, so the first readdir() is the first child rather than ".".
 * AmigaOS has no "." or ".." entries at all, so a caller that filters them --
 * scp.c does -- simply never sees them.
 */
struct DIR
{
    BPTR                  dd_Lock;
    struct FileInfoBlock *dd_Fib;
    struct dirent         dd_Ent;
};

DIR *opendir(const char *path)
{
    DIR *d = (DIR *)AllocVec(sizeof(DIR), MEMF_PUBLIC | MEMF_CLEAR);

    if (d == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }

    d->dd_Fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (d->dd_Fib == NULL)
    {
        FreeVec(d);
        errno = ENOMEM;
        return NULL;
    }

    d->dd_Lock = Lock((CONST_STRPTR)amiga_fix_path(path), ACCESS_READ);
    if (d->dd_Lock == (BPTR)0)
    {
        FreeDosObject(DOS_FIB, d->dd_Fib);
        FreeVec(d);
        errno = ENOENT;
        return NULL;
    }

    if (!Examine(d->dd_Lock, d->dd_Fib) || d->dd_Fib->fib_DirEntryType <= 0)
    {
        UnLock(d->dd_Lock);
        FreeDosObject(DOS_FIB, d->dd_Fib);
        FreeVec(d);
        errno = ENOTDIR;
        return NULL;
    }

    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d == NULL)
    {
        errno = EBADF;
        return NULL;
    }

    if (!ExNext(d->dd_Lock, d->dd_Fib))
        return NULL;                    /* ERROR_NO_MORE_ENTRIES, or a failure */

    d->dd_Ent.d_ino = (long)d->dd_Fib->fib_DiskKey;
    snprintf(d->dd_Ent.d_name, sizeof(d->dd_Ent.d_name), "%s",
             d->dd_Fib->fib_FileName);

    return &d->dd_Ent;
}

int closedir(DIR *d)
{
    if (d == NULL)
    {
        errno = EBADF;
        return -1;
    }

    UnLock(d->dd_Lock);
    FreeDosObject(DOS_FIB, d->dd_Fib);
    FreeVec(d);

    return 0;
}

/*
 * strtod(), so newlib's does not have to be linked.
 *
 * scp calls it in exactly one place -- parsing the -l bandwidth limit -- and
 * newlib's is the single object in libc.a that references
 * _MathIeeeDoubTransBase, a library that is not in Kickstart 3.1 ROM.  Pulling
 * in transcendental maths to read "-l 512" is a poor trade, and defining the
 * symbol here means libc's copy is never pulled: an object on the link line
 * resolves the reference before the archive is searched.
 *
 * Decimal only, with an optional exponent, accumulated in double and scaled by
 * repeated multiplication rather than pow().  No hex floats, no infinity, no
 * NaN; -l takes a small positive number of kbit/s.
 */
double strtod(const char *nptr, char **endptr)
{
    const char *p    = nptr;
    double      val  = 0.0;
    int         neg  = 0;
    int         any  = 0;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;

    if (*p == '+' || *p == '-')
        neg = (*p++ == '-');

    while (*p >= '0' && *p <= '9')
    {
        val = val * 10.0 + (double)(*p++ - '0');
        any = 1;
    }

    if (*p == '.')
    {
        double scale = 1.0;

        p++;
        while (*p >= '0' && *p <= '9')
        {
            val = val * 10.0 + (double)(*p++ - '0');
            scale *= 10.0;
            any = 1;
        }
        val /= scale;
    }

    if (any && (*p == 'e' || *p == 'E'))
    {
        const char *save = p;
        int         eneg = 0;
        int         edig = 0;
        int         exp  = 0;

        p++;
        if (*p == '+' || *p == '-')
            eneg = (*p++ == '-');

        while (*p >= '0' && *p <= '9')
        {
            if (exp < 1000)
                exp = exp * 10 + (*p - '0');
            p++;
            edig = 1;
        }

        if (edig)
        {
            while (exp-- > 0)
            {
                if (eneg)
                    val /= 10.0;
                else
                    val *= 10.0;
            }
        }
        else
        {
            p = save;               /* "1e" is "1" with "e" left over */
        }
    }

    if (endptr != NULL)
        *endptr = (char *)(any ? p : nptr);

    return neg ? -val : val;
}


/*
 * open(), wrapped only to answer /dev/null.
 *
 * scp's main() calls sanitise_stdfd() before anything else, which opens
 * DROPBEAR_PATH_DEVNULL ("/dev/null") and exits if it cannot -- and on AmigaOS
 * it cannot, so scp died with "Couldn't open /dev/null: Success" before reading
 * a byte of protocol.  The Amiga name for that device is NIL:.
 *
 * Every other path is passed through amiga_fix_path(), which collapses the
 * "SYS:/" a Unix-shaped path join produces, and then to newlib's open().  The
 * three std fds are already open when a Shell hands them over, so the fcntl()
 * above returns success for 1 and 2 and dup2() is never reached.
 */
int __wrap_open(const char *path, int flags, ...)
{
    extern int __real_open(const char *path, int flags, ...);

    if (path != NULL && strcmp(path, "/dev/null") == 0)
        return __real_open("NIL:", flags);

    return __real_open(amiga_fix_path(path), flags, 0666);
}


/* ---------------------------------------------------------- the refused --- */

/*
 * scp's client mode only.  do_cmd() reserves two descriptors, makes two pipes,
 * forks and execs `ssh`; none of that exists here, and AmigaOS is not going to
 * grow a fork().  Failing at the first call is what makes `scp file host:path`
 * on the Amiga report an error rather than half-start something.
 *
 * There is a way to support that direction and it is not these functions: scp
 * and dbclient would have to be one binary with the channel wired between them
 * in memory.  docs/RESEARCH.md 79.6.
 */
pid_t fork(void)  { errno = ENOSYS; return -1; }
pid_t vfork(void) { errno = ENOSYS; return -1; }

int execvp(const char *file, char *const argv[])
{
    (void)file; (void)argv;
    errno = ENOSYS;
    return -1;
}

int dup2(int oldfd, int newfd)
{
    (void)oldfd; (void)newfd;
    errno = ENOSYS;
    return -1;
}

pid_t setsid(void) { errno = ENOSYS; return -1; }

int kill(pid_t pid, int sig)
{
    (void)pid; (void)sig;
    errno = ENOSYS;
    return -1;
}

int pipe(int fds[2])
{
    (void)fds;
    errno = ENOSYS;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)status; (void)options;
    errno = ECHILD;
    return -1;
}

/*
 * fcntl() answers the two requests scp makes on a descriptor it did not fork
 * for -- F_GETFL and F_SETFL -- with "no flags set", and refuses the rest.
 * Nothing in -t/-f depends on the answer.
 */
int fcntl(int fd, int cmd, ...)
{
    (void)fd;

    if (cmd == F_GETFL)
        return 0;
    if (cmd == F_SETFL || cmd == F_SETFD)
        return 0;

    errno = ENOSYS;
    return -1;
}

/*
 * signal().  AmigaOS delivers no asynchronous signals to a C handler, so there
 * is nothing to install.  Reporting success is the useful lie: scp installs
 * SIGALRM for the progress meter and SIGPIPE handlers it never needs here, and
 * SIG_ERR from any of them makes it exit.
 */
void (*signal(int sig, void (*func)(int)))(int)
{
    (void)sig;
    return func;
}
