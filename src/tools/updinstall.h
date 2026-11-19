/*
 * updinstall, the decisions of `UpdateAmiNetXDuo INSTALL` that are not I/O:
 * the receipt, where a manifest file lives on this machine, which files an
 * update replaces, the container's member headers, the journal, and the
 * swap itself over a set of file operations the caller supplies.
 *
 * Includes nothing of AmigaOS; compiled natively by
 * src/tools/test/test_updinstall.c, which runs upd_forward() over an
 * in-memory volume with a cut at every step.  The CLI contract it serves is
 * the header of tests/updater/scenarios.tsv.
 *
 * THE RECEIPT, <DOCDIR>/Installed/Update/receipt
 *
 *   "<sha256> <amiga path>", LF, one line per file the installer (or a
 *   confirmed update) placed: 64 lower-case hex digits, one space, the path
 *   as the startup block names it -- LIBS:, DEVS:Networks/, C: and
 *   DEVS:Internet/ for the system layout, AmiNetXDuo:Libs/, Devs/Networks/,
 *   C/ and Devs/Internet/ for the drawer layout.  Written by
 *   `InstallSameFile <receipt> ... RECEIPT`.
 *
 * FILE SELECTION, per file of the installed flavour in the new manifest
 * (receipt hash R, disk hash D, new hash N):
 *
 *   not in the receipt      none: never added (an unselected card's driver)
 *   in it, not on disk      missing deleted: the user removed it
 *   D == N                  skip current
 *   D == R                  replace ours
 *   D differs from both and the AmiNetXDuo mark is in the file:
 *       $VER newer than the update    skip newer: never a downgrade
 *       otherwise                     replace otherbuild (its .old is kept)
 *   no mark                 keep foreign: another vendor's file
 *
 * THE JOURNAL, <DOCDIR>/Installed/Update/journal, LF lines:
 *
 *   format=1
 *   version=<new>  from=<installed>  flavour=<f>      one record each
 *   file=<ours|otherbuild> <path>                     one per file replaced,
 *                                                     the live file's full
 *                                                     path, in swap order
 *   ready                                             every <path>.new is
 *                                                     whole and verified
 *   aside <n>        file n's existing <path>.old goes to <prior>/<n>
 *   done <n>         file n's <path>.new is live
 *
 *   Written as journal.tmp and renamed before the first .new; "ready" and
 *   every line after it are appended.  A last line without its LF is a cut
 *   mid-append and is ignored.  <n> counts the file= lines from 1.  <prior>
 *   is the prior-old drawer beside the journal.
 *
 * RECOVERY from a journal (BOOT):
 *
 *   no "ready"   undo: delete every <path>.new, then the journal.  No live
 *                file was touched.
 *   "ready"      forward: upd_forward() over the file= lines, with the
 *                aside and done lines parsed, finishes every file from what
 *                is on disk; each of its steps can be repeated.  A last line
 *                without its LF is cut off (SetFileSize) before anything is
 *                appended.
 *
 * THE TRIAL, <DOCDIR>/Installed/Update/trial: the journal, renamed once
 * every file is done, with state phase=trial.  BOOT counts the boots in the
 * state; CONFIRM, once the network is up, commits it (upd_commit()); the
 * third boot without that rolls it back (upd_rollback()).  Both append
 * their own lines:
 *
 *   back <n>     rollback: file n's <path>.old is live again
 *   other <n>    commit: file n (otherbuild) moved its .old to
 *                <path>.otherbuild, so its prior .old can come back
 *   kept <n>     commit: file n is finished
 *
 * THE .old RULES, at the commit, never losing a file that was there:
 *
 *   ours, no prior .old         <path>.old (the version replaced) deleted
 *   ours, prior .old            <path>.old deleted, <prior>/<n> back to it
 *   otherbuild, no prior .old   <path>.old (the other build) kept
 *   otherbuild, prior .old      <path>.old to <path>.otherbuild, then
 *                               <prior>/<n> back to <path>.old; when that
 *                               name is taken or cannot be made, both stay
 *                               where they are (upd_commit_stays())
 *
 * and at the rollback: <path>.old live again, then <prior>/<n> back to
 * <path>.old.  The prior copy goes back the way it went aside: Rename, or a
 * copy to <path>.tmp renamed to <path>.old and the copy deleted.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UPDINSTALL_H
#define AMINETXDUO_UPDINSTALL_H

#include "updmanifest.h"

#define UPD_RCPT_MAX        200     /* receipt lines                       */
#define UPD_APATH_MAX       160     /* an Amiga path, with the NUL         */
#define UPD_MARK            "AmiNetXDuo"
#define UPD_SCAN_VER_MAX    48      /* what follows "$VER: ", kept         */

/* ------------------------------------------------------------ receipt --- */

typedef struct UpdRcpt
{
    const char    *path;                /* into the caller's text          */
    unsigned short len;
    unsigned char  sha256[32];
} UpdRcpt;

/* 1 and the lines in `out`, or 0 for a malformed or too long receipt.  An
   empty receipt is malformed: it names nothing placed. */
int upd_receipt_parse(const char *text, unsigned long len, UpdRcpt *out,
                      unsigned int max, unsigned int *n);

/* The index of `path` (`plen` bytes), names compared without case; -1. */
int upd_receipt_find(const UpdRcpt *r, unsigned int n, const char *path,
                     unsigned long plen);

enum { UPD_LAYOUT_SYSTEM = 0, UPD_LAYOUT_DRAWER };

/* Drawer when any line is under AmiNetXDuo:, system otherwise. */
int upd_receipt_layout(const UpdRcpt *r, unsigned int n);

/* A manifest file's path on this machine, as the receipt names it.  0 when
   it does not fit. */
int upd_dest(char *out, unsigned long outlen, int layout, unsigned int cls,
             const char *path, unsigned long plen);

/* ----------------------------------------------------------- selection --- */

typedef enum UpdAct
{
    UPD_ACT_NONE = 0,       /* not ours to touch, not listed               */
    UPD_ACT_REPLACE,
    UPD_ACT_SKIP,
    UPD_ACT_KEEP,
    UPD_ACT_MISSING
} UpdAct;

typedef enum UpdWhy
{
    UPD_WHY_OURS = 0,
    UPD_WHY_CURRENT,
    UPD_WHY_OTHERBUILD,
    UPD_WHY_NEWER,
    UPD_WHY_FOREIGN,
    UPD_WHY_DELETED,
    UPD_WHY_COUNT
} UpdWhy;

/* What a pass over a disk file found: the mark, and the version after the
   first "$VER: <name> ". */
typedef struct UpdScan
{
    unsigned char mark;                 /* 1: UPD_MARK is in the file      */
    unsigned char mpos;                 /* matched bytes of UPD_MARK       */
    unsigned char vpos;                 /* matched bytes of "$VER: "       */
    unsigned char vdone;                /* the $VER text is complete       */
    unsigned char vlen;
    char          ver[UPD_SCAN_VER_MAX];
} UpdScan;

void upd_scan_init(UpdScan *s);
void upd_scan_feed(UpdScan *s, const unsigned char *d, unsigned long n);

/* The version in the $VER string, or 0 when there is none that parses. */
int upd_scan_version(const UpdScan *s, UpdVer *v);

typedef struct UpdSelIn
{
    const unsigned char *receipt;       /* NULL: not in the receipt        */
    const unsigned char *disk;          /* NULL: not on disk               */
    const unsigned char *fresh;         /* the new manifest's hash         */
    const UpdScan       *scan;          /* the disk file's, when on disk   */
    const UpdVer        *update;        /* the update's version            */
} UpdSelIn;

void upd_select(const UpdSelIn *in, unsigned char *act, unsigned char *why);

const char *upd_act_name(unsigned int a);   /* "replace" ...; NULL past    */
const char *upd_why_name(unsigned int w);   /* "ours" ...                  */

/* ----------------------------------------------------------- container --- */

/*
 * A member header of the stored container (dist/lhapack.py: header level
 * 1, -lh0-, the directory in an extended header 0x02).  upd_lha_base()
 * reads the base header, `n` bytes from its first; it needs the first byte
 * alone to say how long it is.  Returns the base header's length (>= 24),
 * 0 for the end of the archive, -1 for a header that is not one, or -2
 * when `n` is short of the length (call again with that many).
 */
#define UPD_LHA_HEAD_MAX    257
#define UPD_LHA_EXT_MAX     512     /* extended headers of one member      */

typedef struct UpdLha
{
    unsigned long skip;                 /* extended headers + data         */
    unsigned long size;                 /* data, stored as it is           */
    unsigned int  ext_first;            /* size of the first extended one  */
    char          name[UPD_PATH_MAX + 32];  /* directory/name, '/' between */
    unsigned int  name_len;
} UpdLha;

long upd_lha_base(const unsigned char *h, unsigned long n, UpdLha *m);

/* The extended headers, `n` bytes; the directory goes before the name.
   1, or 0 for a malformed chain or a name that does not fit. */
int upd_lha_ext(const unsigned char *e, unsigned long n, UpdLha *m);

/* The manifest file a member name ("C/ping") is, or -1. */
int upd_member_file(const char *name, unsigned long nlen,
                    const UpdFile *files, unsigned int nfiles);

/* FFS blocks a file of `size` bytes takes, counted as OFS (data blocks of
   `bpb` - 24 bytes), its header and its extension blocks. */
unsigned long upd_blocks(unsigned long size, unsigned long bpb);

/* The container fetch, without its I/O.  Only a final 2xx opens the sink,
   but an error status's body still reaches it: that body is neither
   written nor counted. */
enum { UPD_DL_OK = 0, UPD_DL_NETWORK, UPD_DL_SPACE, UPD_DL_LONG };

/* A body chunk of `n` bytes after `total`: UPD_DL_OK to write it, or the
   reason to stop -- UPD_DL_NETWORK for a sink not opened, UPD_DL_LONG
   past the manifest's `size`. */
int upd_dl_chunk(int opened, unsigned long total, unsigned long n,
                 unsigned long size);

/* What the fetch came to: `status` the final HTTP status, `whole` the
   fetch returned RETURN_OK, `wfail` a write or the close failed.  The
   status first: an error answer is UPD_DL_NETWORK whatever its body did. */
int upd_dl_verdict(long status, int whole, int wfail, int toolong,
                   unsigned long total, unsigned long size);

/* 1 for an IoErr() that says the object is not there (205,
   ERROR_OBJECT_NOT_FOUND).  Any other failure to lock or read is not
   "absent": the caller refuses. */
int upd_ioerr_absent(long ioerr);

/* a + b, held at ~0UL rather than wrapped. */
unsigned long upd_sat_add(unsigned long a, unsigned long b);

/* id_NumBlocks - id_NumBlocksUsed, 0 rather than wrapped below it. */
unsigned long upd_free_blocks(long num, long used);

/* ------------------------------------------------------------- journal --- */

typedef struct UpdJEntry
{
    const char    *path;                /* the live file's full path       */
    unsigned short len;
    unsigned char  why;                 /* UPD_WHY_OURS | _OTHERBUILD      */
    unsigned char  aside;               /* an "aside <n>" line             */
    unsigned char  done;                /* a "done <n>" line               */
    unsigned char  back;                /* a "back <n>" line (rollback)    */
    unsigned char  other;               /* an "other <n>" line (commit)    */
    unsigned char  kept;                /* a "kept <n>" line (commit)      */
    unsigned char  staged;              /* this run wrote <path>.new       */
} UpdJEntry;

typedef struct UpdJournal
{
    unsigned char ready;
    unsigned char flavour;
    UpdVer        version;
    UpdVer        from;
    unsigned int  nfiles;
} UpdJournal;

/* 1 and the journal, or 0 for one that is not one. */
int upd_journal_parse(const char *text, unsigned long len, UpdJournal *j,
                      UpdJEntry *e, unsigned int max);

/*
 * What INSTALL's cleanup does with the journal it finds: `text` is NULL
 * when it could not be read, and `absent` says the file is not there
 * (ERROR_OBJECT_NOT_FOUND).  UPD_STALE_NONE: no journal; UPD_STALE_UNDO:
 * a preparation that stopped before "ready", whose .new files go;
 * UPD_STALE_PENDING: anything else -- ready, unreadable, not a journal --
 * which is refused with nothing removed.
 */
enum { UPD_STALE_NONE = 0, UPD_STALE_UNDO, UPD_STALE_PENDING };

int upd_stale_journal(const char *text, unsigned long len, int absent,
                      UpdJournal *j, UpdJEntry *e, unsigned int max);

/* The file operations the swap runs on.  Every one returns 1 for done.
   rename fails when `to` exists or the two are on different volumes;
   remove of a name that is not there is done. */
typedef struct UpdFsOps
{
    void *ctx;
    int (*exists)(void *ctx, const char *path);     /* 1, 0, or < 0:  */
                                        /* it cannot be said (a DOS error) */
    int (*rename)(void *ctx, const char *from, const char *to);
    int (*remove)(void *ctx, const char *path);
    int (*copy)(void *ctx, const char *from, const char *to);
    int (*journal)(void *ctx, const char *line);    /* appends line + LF   */
    int (*cut)(void *ctx);              /* a cut point; 0 stops the swap   */
} UpdFsOps;

/*
 * The swap, and the forward recovery: per entry not done, in order --
 *
 *   an existing <path>.old to <prior>/<n> ("aside <n>" first; Rename, or a
 *   copy to <prior>/<n>.tmp renamed to <prior>/<n> and the .old deleted)
 *   <path> to <path>.old
 *   <path>.new to <path>
 *   "done <n>"
 *
 * each step taken only when the disk says it is still to do, and a cut
 * point after each.  A prior-old/<n> that is there before "aside <n>" is
 * an earlier update's: the swap stops there (0) and deletes no .old;
 * PREPARE refuses it first.  <path>.new and .old are upd_sidecar()'s.
 * Returns 1 when every entry is done, 0 when an
 * operation failed or a cut stopped it (*failed, if not NULL, is the
 * entry), -1 for an entry whose live and .new files are both gone.
 */
int upd_forward(UpdJEntry *e, unsigned int n, const char *prior,
                const UpdFsOps *ops, unsigned int *failed);

/* The cut points upd_forward() passes for `n` entries of which `olds` have
   a .old to move aside. */
unsigned int upd_forward_steps(unsigned int n, unsigned int olds);

/*
 * The rollback over the trial's entries: per entry, the new <path> deleted
 * and <path>.old renamed to it, "back <n>", then an aside's <prior>/<n> back
 * to <path>.old; each step taken only when the disk says it is still to do,
 * a cut point after each.  Returns 1 when every entry is back, 0 when an
 * operation failed or a cut stopped it, -1 for an entry with neither a .old
 * nor a live file.
 */
int upd_rollback(UpdJEntry *e, unsigned int n, const char *prior,
                 const UpdFsOps *ops, unsigned int *failed);

/* The commit over the trial's entries, by the .old rules above; "kept <n>"
   after each.  1 when every entry is kept, 0 as upd_rollback(). */
int upd_commit(UpdJEntry *e, unsigned int n, const char *prior,
               const UpdFsOps *ops, unsigned int *failed);

/* After upd_commit(): 1 when entry e left its other build at <path>.old and
   its prior .old in <prior>/<n>, because <path>.otherbuild could not be
   made. */
int upd_commit_stays(const UpdJEntry *e);

/* The length of `text` up to and with its last LF: what a journal keeps
   when its last line was cut short. */
unsigned long upd_journal_trim(const char *text, unsigned long len);

/*
 * S:AmiNetXDuo-Files with `section` -- a block from "; BEGIN
 * UpdateAmiNetXDuo" to "; END UpdateAmiNetXDuo" and its LF -- in place of
 * the one `old` holds, or after its first FailAt line, or first.  The
 * length, or 0 when it does not fit in `outlen`.
 */
#define UPD_REC_BEGIN       "; BEGIN UpdateAmiNetXDuo"
#define UPD_REC_END         "; END UpdateAmiNetXDuo"
unsigned long upd_record_merge(const char *old, unsigned long oldlen,
                               const char *section, unsigned long seclen,
                               char *out, unsigned long outlen);

/* 1 when the journal holds entry `i` (from 0) and upd_forward() names all
   it needs -- <path>, .new, .old, <prior>/<n> and its .tmp -- within
   UPD_APATH_MAX; 0 otherwise.  PREPARE asks it of every file before it
   stages anything. */
int upd_swap_fits(const UpdJEntry *e, unsigned int i, const char *prior);

/* 1 when no entry's <path>.new is there; 0 and *bad, the first that is:
   a file PREPARE did not write and must not overwrite or delete. */
int upd_new_free(const UpdJEntry *e, unsigned int n, const UpdFsOps *ops,
                 unsigned int *bad);

/* Removes <path>.new of every entry this run staged, and no other. */
void upd_unstage_new(UpdJEntry *e, unsigned int n, const UpdFsOps *ops);

/* 1 when every entry was staged: each selected member came, and once
   (the extraction refuses a second one before it writes it). */
int upd_all_staged(const UpdJEntry *e, unsigned int n);

/* ------------------------------------------------------------- names --- */

/*
 * Every name the updater makes, in the Update drawer and beside a target,
 * fits UPD_FNAME_MAX: a classic OFS/FFS volume holds no longer one, and
 * cuts or refuses it -- and a cut name escapes a cleanup by name.  The
 * container is stored as UPD_N_CONTAINER whatever the mirror calls it.
 */
#define UPD_FNAME_MAX       30
#define UPD_N_JOURNAL       "journal"
#define UPD_N_JOURNAL_TMP   "journal.tmp"
#define UPD_N_TRIAL         "trial"
#define UPD_N_RECEIPT       "receipt"
#define UPD_N_RECEIPT_NEXT  "receipt-next"
#define UPD_N_STATE         "state"
#define UPD_N_STATE_TMP     "state.tmp"
#define UPD_N_PRIOR         "prior-old"     /* a drawer: <n>, <n>.tmp      */
#define UPD_N_LOCK          "lock"
#define UPD_N_LOG           "log"
#define UPD_N_CONTAINER     "container.update.lha"
#define UPD_N_CONTAINER_EXT ".update.lha"   /* every container, cleaned    */

/*
 * <path> and `ext` (".new", ".old") into `out`: the sidecar the swap uses.
 * A last component too long for UPD_FNAME_MAX with `ext` is cut first
 * (C:CreateAmiNetXDuoStatusReport is 28: its .new is
 * C:CreateAmiNetXDuoStatusRepo.new).  0 when it does not fit `cap`.
 */
int upd_sidecar(char *out, unsigned long cap, const char *path,
                unsigned long len, const char *ext);

/* 1 when no two entries' sidecars are one name (two long names in one
   drawer cut alike); 0 and *bad, the later of the two. */
int upd_sidecars_distinct(const UpdJEntry *e, unsigned int n, unsigned int *bad);

/* ---------------------------------------------------------------- state --- */

/*
 * <DOCDIR>/Installed/Update/state with phase, version, boots and flavour
 * set and every other line of `old` kept, into `out`.  The length, or 0
 * when it does not fit.
 */
unsigned long upd_state_merge(const char *old, unsigned long oldlen,
                              const char *phase, const char *version,
                              unsigned long boots, const char *flavour,
                              char *out, unsigned long outlen);

/* The value of the state line "<key>=" (key without case) into `out`, NUL
   ended; 1, or 0 when there is none or it does not fit. */
int upd_state_get(const char *text, unsigned long len, const char *key,
                  char *out, unsigned long outlen);

/* 1 when an installed update waits for CONFIRM: the trial file is there
   (`trial`), or the state (`len` bytes, NULL for none) says phase=trial.
   INSTALL then refuses before it removes anything: receipt-next is
   CONFIRM's. */
int upd_trial_active(int trial, const char *state, unsigned long len);

/* What INSTALL's cleanup asks: 1 to refuse pending.  The trial file
   counts as gone only when `trial_absent` (ERROR_OBJECT_NOT_FOUND), the
   state only when `state_absent`; a state that could not be read (NULL,
   not absent) refuses, as an unreadable journal does. */
int upd_trial_refuse(int trial_absent, const char *state, unsigned long len,
                     int state_absent);

/*
 * InstallSameFile RECEIPT: what it does with FIRST, by how its Open() went
 * (`opened`, else `absent` for ERROR_OBJECT_NOT_FOUND) and the lines it
 * has (`have` bytes, the kept ones and this call's).  Any other open
 * failure is an error, not an empty receipt; no lines at all is done with
 * nothing to write (a drawer of this flavour with none of its files).
 */
enum { UPD_RW_ERROR = 0, UPD_RW_NONE, UPD_RW_WRITE };

static inline int upd_rcpt_write_plan(int opened, int absent, unsigned long have)
{
    if (!opened && !absent)
        return UPD_RW_ERROR;
    return have == 0 ? UPD_RW_NONE : UPD_RW_WRITE;
}

/*
 * The Update drawer's lock, <DOCDIR>/Installed/Update/lock, opened
 * MODE_NEWFILE (an exclusive lock) for the whole of an INSTALL, BOOT or
 * CONFIRM and deleted when it is closed.  The filesystem drops the lock
 * when the machine restarts, so a run that crashed blocks nothing after
 * a reboot; the file it leaves is reopened.  What a failed open means,
 * by its IoErr(): UPD_LOCK_BUSY (202, ERROR_OBJECT_IN_USE) another run
 * holds it, UPD_LOCK_WP (214, 223: write-protected) the drawer's volume
 * is read-only, UPD_LOCK_OTHER anything else.
 */
enum { UPD_LOCK_BUSY = 1, UPD_LOCK_WP, UPD_LOCK_OTHER };

int upd_lock_why(long ioerr);

/* The state SWAP writes, phase=trial and every other line of `old` kept;
   merged before PREPARE.  0 -- refused, nothing dropped -- when `old` is
   NULL and not `absent` (it could not be read, or is over the size), or
   when the merge does not fit. */
unsigned long upd_state_next(const char *old, unsigned long oldlen, int absent,
                             const char *version, const char *flavour,
                             char *out, unsigned long outlen);

/* Lower-case hex of 32 bytes into `out`, 65 bytes with the NUL. */
void upd_hex32(const unsigned char *b, char *out);

#endif /* AMINETXDUO_UPDINSTALL_H */
