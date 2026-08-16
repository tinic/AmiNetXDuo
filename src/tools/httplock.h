/*
 * The WebDAV lock table, RFC 4918 §6 and §7.
 *
 * Split out of httpd.c so the rules can be asked questions without a socket.
 * Everything here is a decision about paths, tokens and a clock.  The 423 that
 * follows a refusal, and the XML that goes with it, stay in httpd.c.
 *
 * `now` is a parameter rather than a call to the clock so a test can place a
 * lock's expiry on either side of it.  Callers in httpd.c pass httpd_now().
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPLOCK_H
#define AMINETXDUO_HTTPLOCK_H

#include "httppath.h"

#define HTTPD_LOCK_MAX          8
#define HTTPD_TOKEN_MAX        64       /* "opaquelocktoken:...", as text    */
#define HTTPD_OWNER_MAX       128

typedef struct HttpLock
{
    char          path[HTTP_PATH_MAX];  /* the AmigaOS path it covers        */
    char          token[HTTPD_TOKEN_MAX];
    char          owner[HTTPD_OWNER_MAX];
    unsigned long expires;              /* in the caller's clock             */
    unsigned long timeout;              /* what was granted, for the reply   */
    unsigned char depth;                /* 1 when it covers everything below */
    unsigned char used;
} HttpLock;

/*
 * Expiry is lazy, on the next question anybody asks.  There is no timer here,
 * and an expired lock nobody asks about costs nothing.  Every lookup below
 * runs it first, so a caller does not have to.
 */
void      httplock_expire(unsigned long now);

/* The lock covering `path`: its own, or a drawer's above it. */
HttpLock *httplock_on(const char *path, unsigned long now);

/* A lock on this exact path, expired ones included: what LOCK refreshes. */
HttpLock *httplock_exact(const char *path);

HttpLock *httplock_by_token(const char *token, unsigned long now);

/* An unused slot, or NULL when the table is full. */
HttpLock *httplock_free_slot(void);

/*
 * Every lock rooted at `path` or below it, gone.  RFC 4918 §9.6: a DELETE that
 * succeeded MUST destroy them, and a MOVE's source is a DELETE of that name.
 */
void      httplock_drop(const char *path);

/*
 * Does `l` cover `path`?  Its own resource, or, when it was taken with
 * Depth: infinity, anything below it.
 *
 * Both UNLOCK and a LOCK refresh have to ask this.  Holding a token is not
 * enough on its own: the table is global, so a token alone lets any client
 * operate on any lock in it through any address.
 */
int       httplock_covers(const HttpLock *l, const char *path);

/* Does a request carrying these two If: tokens hold `l`?  A NULL `l` is held
   by everybody. */
int       httplock_held(const char *tok0, const char *tok1, const HttpLock *l);

/* The three questions a write goes through.  Nonzero means allowed. */
int       httplock_allows(const char *tok0, const char *tok1,
                          const char *path, unsigned long now);

/*
 * `path` and everything under it: a Depth: infinity method must not step over
 * a name locked underneath it.
 */
int       httplock_allows_tree(const char *tok0, const char *tok1,
                               const char *path, unsigned long now);

/*
 * The lock on the drawer that holds this address.
 *
 * RFC 4918 §7.1: a lock on a collection, at any depth including 0, locks that
 * collection's internal member namespace, so adding a member or taking one
 * away needs the token even when the member itself is not locked.  Changing an
 * existing member's content does not, which is why this is asked only by the
 * methods that create or remove a name.
 *
 * httplock_on() answers about the address itself and about anything under a
 * deep lock.  Neither of those covers a Depth: 0 lock one level up.
 */
int       httplock_allows_parent(const char *tok0, const char *tok1,
                                 const char *path, unsigned long now);

/* Empties the table.  For tests, since httpd starts with it zeroed. */
void      httplock_reset(void);

#endif /* AMINETXDUO_HTTPLOCK_H */
