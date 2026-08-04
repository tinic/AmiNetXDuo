/*
 * clients/compat/amiga_compat.h, prototypes for the shims in this directory
 * that newlib's headers do not declare.
 *
 * amiga_posix.c supplies functions the toolchain's libc.a does not have.
 * Defining them is enough to link, but a caller that has never seen a
 * prototype is an implicit declaration, which this toolchain treats as an
 * error.  The declaration has to reach the client's own sources.
 *
 * Three ways to do that: patch the client, drop a replacement <time.h> into
 * the shim include directory, or force-include a header of our own.  The first
 * has to be rebased on every version bump; the second shadows a real header
 * for every translation unit that includes it.  This is the third, and it is
 * why clients/amiga-client.sh already carries -include sys/types.h.
 *
 * Keep this file cheap: it is parsed by every translation unit of every ported
 * client, so it declares and includes as little as it can.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CLIENTS_COMPAT_H
#define AMINETXDUO_CLIENTS_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Declared, not included: <time.h> is not cheap and a pointer parameter does
   not need the definition.  At file scope rather than in the prototype, so it
   is the same struct timespec <time.h> later defines and not a new one scoped
   to the parameter list. */
struct timespec;

/* Collapse ":/" to ":", see amiga_posix.c.  Returns its argument unchanged
   when there is nothing to fix, and otherwise a pointer to a static buffer,
   so the result is only valid until the next call. */
const char *amiga_fix_path(const char *path);

/* Delay().  One tick of resolution, rounds up, never returns EINTR. */
int nanosleep(const struct timespec *req, struct timespec *rem);

/* Not ours: newlib's libc.a defines clearenv() (lib_a-environ.o) but never
   declares it, <stdlib.h> puts it behind a visibility guard this toolchain
   does not set.  Declared here rather than defined, because defining it is a
   multiple definition at link time.

   Do not write one using environ: <unistd.h> makes that a macro for
   (*environ_ptr), and environ_ptr is declared but defined nowhere in the
   toolchain.  newlib's own clearenv() touches the _environ array directly. */
int clearenv(void);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_CLIENTS_COMPAT_H */
