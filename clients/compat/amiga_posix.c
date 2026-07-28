/*
 * clients/compat -- the POSIX calls a ported Unix network client references
 * and this toolchain's newlib does not define.
 *
 * curl, wget and every other Unix client were written against a libc that has
 * stat(), mkdir(), unlink(), isatty() and gettimeofday().  The newlib in this
 * m68k-amigaos toolchain has a thin syscall layer: libc.a's lib_a-dummy.o
 * defines _fstat, _unlink, _lseek, _getpid and _kill and nothing else, with no
 * non-underscore wrappers over any of them.  So a link of the curl command
 * line tool (libcurl itself is clean) fails with this list:
 *
 *     fstat  stat  ftruncate  isatty  mkdir  unlink  _gettimeofday  _link
 *
 * Each has a two-line AmigaOS answer through dos.library, so this is a shim
 * and not a port.  It lives in clients/ and not src/: nothing in the stack
 * itself uses newlib, and a bsdsocket.library that dragged dos.library file
 * calls in would be a defect.
 *
 *   stat()/lstat()   Lock() + Examine().  st_mode, st_size, st_mtime and
 *                    st_nlink are real.  AmigaOS has no symbolic links in the
 *                    POSIX sense, so lstat() is stat().
 *   fstat()          newlib's _fstat() returns 0 and writes nothing to the
 *                    struct, which is worse than failing -- callers read
 *                    uninitialised st_mode.  This one zeroes the struct,
 *                    asks _isatty() what kind of thing the descriptor is, and
 *                    measures a regular file with _lseek().
 *   ftruncate()      SetFileSize() needs a BPTR and only a newlib descriptor
 *                    is available, so this fails with EINVAL.  curl calls it
 *                    in one place -- truncating a partially written --output
 *                    file after a failed resume -- and handles the failure.
 *   link()           AmigaOS has MakeLink(), but hard links are a filesystem
 *                    option most Amiga volumes do not have.  ENOSYS.
 *   gettimeofday()   DateStamp().  Resolution is one tick (1/50 s), and the
 *                    epoch conversion is the AmigaOS one: 1978-01-01, which
 *                    is 2,922 days after 1970-01-01.  A machine with a dead
 *                    RTC battery therefore reports 1978 here, as
 *                    tls.library's clock check already assumes.
 *   nanosleep()      Delay(), so the resolution is one tick (20 ms) and a
 *                    request is rounded up to the next whole tick.  A caller
 *                    asking for less than a tick still loses one.  It never
 *                    returns EINTR, so the standard
 *                    while (nanosleep(...) == -1 && errno == EINTR) loop runs
 *                    once.  rem is zeroed rather than left alone.
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
#include <time.h>               /* struct timespec, for nanosleep() */
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* newlib's own syscall layer, such as it is.  Declared here rather than
   included, because <unistd.h> does not know about the underscore names. */
extern int  _isatty(int fd);
extern int  _unlink(const char *path);
extern long _lseek(int fd, long offset, int whence);

/* Seconds between 1970-01-01 and the AmigaOS epoch, 1978-01-01. */
#define AMIGA_EPOCH_OFFSET  (2922L * 86400L)


/* --------------------------------------------------------------- path --- */

/*
 * Every ported program builds a path by writing "%s/.ssh/..." after whatever
 * getpwnam() gave it as a home directory.  On AmigaOS the home is a device or
 * assign name ending in a colon, so that produces
 *
 *     SYS:/.ssh/authorized_keys
 *
 * and a colon followed by a slash means the parent of the root, which does not
 * exist, so the Lock() fails and the program reports that the file is missing.
 * The correct spelling is SYS:.ssh/authorized_keys.
 *
 * Collapsing ":/" to ":" is safe because the sequence has no other valid
 * meaning in an AmigaOS path: a device name is always followed directly by the
 * thing inside it.  Done once, where paths enter, rather than asked of every
 * caller.
 *
 * The buffer is static and this is not re-entrant, for the same reason the
 * FileInfoBlock below is: a Shell command's stack is 4 KB and nothing here
 * opens two files at once.
 */
static char amiga_path_buf[512];

const char *amiga_fix_path(const char *path)
{
    const char *colon;
    size_t      head;

    if (path == NULL)
        return path;

    colon = strchr(path, ':');
    if (colon == NULL || colon[1] != '/')
        return path;

    head = (size_t)(colon - path) + 1;           /* through the colon */
    if (head + strlen(colon + 2) + 1 > sizeof(amiga_path_buf))
        return path;                             /* too long to fix; leave it */

    memcpy(amiga_path_buf, path, head);
    strcpy(amiga_path_buf + head, colon + 2);    /* skip the '/' */
    return amiga_path_buf;
}


/* --------------------------------------------------------------- stat --- */

/*
 * Examine() wants a FileInfoBlock, which must be longword aligned.  A static
 * one keeps it off a Shell command's 4 KB stack (260 bytes is a lot of it)
 * and this shim is not re-entrant for any other reason either.
 */
static struct FileInfoBlock stat_fib __attribute__((aligned(4)));

static void stat_fill(struct stat *st, const struct FileInfoBlock *fib)
{
    memset(st, 0, sizeof(*st));

    if (fib->fib_DirEntryType > 0)
        st->st_mode = S_IFDIR | 0755;
    else
        st->st_mode = S_IFREG | 0644;

    /* fib_Protection is active low for the RWED bits: a set bit means the
       operation is forbidden.  Getting this backwards makes every file look
       unreadable. */
    if ((fib->fib_Protection & FIBF_READ) != 0)
        st->st_mode &= ~(mode_t)0444;
    if ((fib->fib_Protection & FIBF_WRITE) != 0)
        st->st_mode &= ~(mode_t)0222;

    st->st_size  = (off_t)fib->fib_Size;
    st->st_nlink = 1;

    st->st_mtime = (time_t)AMIGA_EPOCH_OFFSET
                 + (time_t)fib->fib_Date.ds_Days   * 86400L
                 + (time_t)fib->fib_Date.ds_Minute * 60L
                 + (time_t)(fib->fib_Date.ds_Tick / TICKS_PER_SECOND);
    st->st_atime = st->st_mtime;
    st->st_ctime = st->st_mtime;

    st->st_blksize = 512;
    st->st_blocks  = (blkcnt_t)((fib->fib_Size + 511L) / 512L);
}

int stat(const char *path, struct stat *st)
{
    BPTR lock;

    if (path == NULL || st == NULL)
    {
        errno = EFAULT;
        return -1;
    }

    lock = Lock((CONST_STRPTR)amiga_fix_path(path), SHARED_LOCK);
    if (lock == (BPTR)0)
    {
        errno = ENOENT;
        return -1;
    }

    if (!Examine(lock, &stat_fib))
    {
        UnLock(lock);
        errno = EIO;
        return -1;
    }

    UnLock(lock);
    stat_fill(st, &stat_fib);
    return 0;
}

int lstat(const char *path, struct stat *st)
{
    return stat(path, st);
}

/*
 * newlib's _fstat() returns success and leaves the caller's struct alone, so a
 * caller that checks S_ISREG() is reading whatever was on the stack.  This one
 * answers the two questions a client asks -- "is this a terminal" and "how big
 * is it" -- and says nothing it does not know.
 */
int fstat(int fd, struct stat *st)
{
    long here, end;

    if (st == NULL)
    {
        errno = EFAULT;
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->st_nlink  = 1;
    st->st_blksize = 512;

    if (_isatty(fd))
    {
        st->st_mode = S_IFCHR | 0666;
        return 0;
    }

    st->st_mode = S_IFREG | 0644;

    here = _lseek(fd, 0L, SEEK_CUR);
    if (here >= 0)
    {
        end = _lseek(fd, 0L, SEEK_END);
        if (end >= 0)
        {
            st->st_size  = (off_t)end;
            st->st_blocks = (blkcnt_t)((end + 511L) / 512L);
            (void)_lseek(fd, here, SEEK_SET);
        }
    }

    return 0;
}


/* ------------------------------------------------------ the easy ones --- */

int isatty(int fd)
{
    return _isatty(fd);
}

int unlink(const char *path)
{
    return _unlink(amiga_fix_path(path));
}

int mkdir(const char *path, mode_t mode)
{
    BPTR lock;

    (void)mode;                 /* AmigaOS has no umask */

    if (path == NULL)
    {
        errno = EFAULT;
        return -1;
    }

    lock = CreateDir((CONST_STRPTR)amiga_fix_path(path));
    if (lock == (BPTR)0)
    {
        errno = (IoErr() == ERROR_OBJECT_EXISTS) ? EEXIST : EACCES;
        return -1;
    }

    UnLock(lock);
    return 0;
}

/*
 * SetFileSize() takes a BPTR and only a newlib descriptor is available, with
 * no published way from one to the other, so this fails.  curl's single caller
 * (post_per_transfer(), trimming a --output file after a failed resume) checks
 * the return value and reports it.
 */
int ftruncate(int fd, off_t length)
{
    (void)fd;
    (void)length;
    errno = EINVAL;
    return -1;
}

/* Referenced by newlib's internals, not by any client directly. */
int _link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}

int link(const char *from, const char *to)
{
    return _link(from, to);
}


/* ------------------------------------------------------------- clock --- */

int _gettimeofday(struct timeval *tv, void *tz)
{
    struct DateStamp ds;

    (void)tz;

    if (tv == NULL)
    {
        errno = EFAULT;
        return -1;
    }

    DateStamp(&ds);

    tv->tv_sec  = (time_t)AMIGA_EPOCH_OFFSET
                + (time_t)ds.ds_Days   * 86400L
                + (time_t)ds.ds_Minute * 60L
                + (time_t)(ds.ds_Tick / TICKS_PER_SECOND);
    tv->tv_usec = (suseconds_t)((ds.ds_Tick % TICKS_PER_SECOND)
                                * (1000000L / TICKS_PER_SECOND));
    return 0;
}

int gettimeofday(struct timeval *__restrict tv, struct timezone *__restrict tz)
{
    return _gettimeofday(tv, (void *)tz);
}


/* ------------------------------------------------------------- sleep --- */

/*
 * dropbear's server sleeps here for 250-350 ms after a failed password, so
 * that a rejected user and a rejected password take the same time.  One tick
 * of granularity leaves five or six distinct outcomes across that range
 * instead of a continuum, still finer than the variance a 14 MHz machine's
 * scheduler and an Ethernet round trip already add.
 *
 * Delay(0) does not delay, so a request that rounds down to nothing is given a
 * single tick.
 */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    long ticks;

    if (req == NULL || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L
        || req->tv_sec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (rem != NULL)
    {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }

    /* Round up: 1 tick is 20 ms, so 1 tick is 20,000,000 ns. */
    ticks = (long)req->tv_sec * TICKS_PER_SECOND
          + (long)((req->tv_nsec + 19999999L) / 20000000L);

    if (ticks <= 0)
        ticks = 1;

    Delay((ULONG)ticks);
    return 0;
}

