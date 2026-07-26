/*
 * clients/dropbear/localoptions.h -- what is compiled into the AmigaOS
 * dbclient, and why each thing is off.
 *
 * Dropbear reads this from the BUILD directory if it exists; clients/dropbear/
 * build.sh copies it there.  Everything not named here keeps the value in
 * third_party/dropbear/src/default_options.h, so this file is the whole
 * difference between upstream's defaults and ours.
 *
 * THE THINGS THAT NEED fork(), AND THEREFORE CANNOT EXIST HERE
 *
 *   DROPBEAR_CLI_PROXYCMD   -J: runs a helper program and talks to it over a
 *                           pipe.  fork() + exec() + pipe(), none of which
 *                           AmigaOS has.
 *   DROPBEAR_CLI_NETCAT     -B: same machinery.
 *   DROPBEAR_CLI_ASKPASS_HELPER
 *                           forks $SSH_ASKPASS.  clients/dropbear/
 *                           amiga_dropbear.c has a real getpass() over
 *                           dos.library instead, which is better anyway.
 *
 *   With these three off, a linked dbclient calls fork() from nowhere.  That
 *   is the finding, not the workaround: the SSH CLIENT does not need a
 *   process model, only the server does.
 *
 * THE THINGS THAT NEED A UNIX SOCKET OR A LISTENING SOCKET
 *
 *   DROPBEAR_CLI_AGENTFWD   talks to ssh-agent over AF_UNIX.  bsdsocket.library
 *                           has no AF_UNIX and there is no agent to talk to.
 *   DROPBEAR_CLI_LOCALTCPFWD / REMOTETCPFWD
 *                           -L / -R.  These do work in principle -- listen()
 *                           and accept() are published vectors -- and they are
 *                           off only because nothing has tested them yet.  This
 *                           is the first line to turn back on.
 *
 * THE THINGS THAT ARE TOO BIG OR TOO SLOW FOR A 14 MHz 68020
 *
 *   DROPBEAR_SNTRUP761 / DROPBEAR_MLKEM768
 *                           post-quantum hybrid key exchange.  ML-KEM-768 is
 *                           ~2,400 bytes of public key and a lattice
 *                           multiplication; sntrup761 is worse.  Both are
 *                           preferred over curve25519 when offered, so leaving
 *                           them in would make every handshake pay for them.
 *                           OpenSSH still offers curve25519-sha256, so nothing
 *                           is lost in reachability.
 *
 *   DROPBEAR_SMALL_CODE 0   the opposite of upstream's default, on purpose.
 *                           docs/RESEARCH.md §18 measured this machine: it has
 *                           NO DATA CACHE, a 4 KB table costs the same per
 *                           lookup as a 1 KB one (159.845 ns against 159.847),
 *                           and the small-table layouts pay rotates to save a
 *                           footprint that is free here.  Small code is a
 *                           pessimisation on this part.
 *
 * SERVER OPTIONS APPEAR HERE BECAUSE sysoptions.h CHECKS THEM UNCONDITIONALLY
 *
 *   DROPBEAR_SVR_PASSWORD_AUTH must be 0: it is `#error ... requires crypt()`
 *   and this toolchain has no crypt().  DROPBEAR_SVR_MULTIUSER must be 0: it
 *   needs setresgid().  Both fire while building dbclient, which compiles none
 *   of the server.  They say nothing about what a future server would do.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DROPBEAR_AMIGA_LOCALOPTIONS_H
#define DROPBEAR_AMIGA_LOCALOPTIONS_H

/* needs fork()/exec()/pipe() */
#define DROPBEAR_CLI_PROXYCMD 0
#define DROPBEAR_CLI_NETCAT 0
#define DROPBEAR_CLI_ASKPASS_HELPER 0

/* needs AF_UNIX, or is simply untested here */
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0

/* too big / too slow for this machine */
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_SK_KEYS 0
#define DROPBEAR_DSS 0
#define DROPBEAR_X11FWD 0

/* no data cache: see the note above */
#define DROPBEAR_SMALL_CODE 0

/*
 * THE OPTIMISTIC KEY EXCHANGE GUESS, WHICH COSTS THIS MACHINE A WHOLE SCALAR
 * MULTIPLICATION AND SAVES IT ONE ROUND TRIP.
 *
 * With DROPBEAR_KEX_FIRST_FOLLOWS on (upstream's default), dbclient sends a
 * KEXDH_INIT for its own first-preference kex before it has seen the server's
 * KEXINIT, betting the server prefers the same one.  When the bet is wrong the
 * server discards the packet -- `proposal mismatch: my mlkem768x25519-sha256
 * peer curve25519-sha256`, `skipped packet (type 30)` in its log -- and
 * cli-kex.c's send_msg_kexdh_init() runs again, starting with
 * cli_kex_free_param() and a FRESH gen_kexcurve25519_param().
 *
 * A MODERN OpenSSH ALWAYS WINS THAT BET AGAINST US, because it prefers an
 * ML-KEM hybrid and we do not offer one (see above).  So the guess is not a
 * gamble here, it is a guaranteed loss.
 *
 * MEASURED, on the emulated 14 MHz A1200, same binary otherwise, same server,
 * timed by the guest's own clock:
 *
 *     guess on   96.06  95.88  96.06  95.92 s   (mean 95.98)
 *     guess off  84.18  83.96 s                (mean 84.07)
 *
 * Just under twelve seconds, which is one curve25519 scalar multiplication on
 * this part.  What the guess buys in exchange is one round trip, which on any
 * link this machine can saturate is a few milliseconds.
 */
#define DROPBEAR_KEX_FIRST_FOLLOWS 0

/* no ~/.ssh/config to read, and no MOTD worth printing over a serial-speed
   link */
#define DROPBEAR_USE_SSH_CONFIG 0
#define DO_MOTD 0

/*
 * THE ENTROPY DEVICE.
 *
 * dbrandom.c's seedrandom() opens this path, reads 32 bytes and
 * dropbear_exit()s if it cannot.  There is no build without one.  AmigaOS has
 * no /dev/urandom, so the name is changed to something shaped like an AmigaOS
 * device -- nothing here pretends to be Unix -- and clients/dropbear/
 * amiga_dropbear.c intercepts open() for exactly this string and answers it
 * from src/common/ami_random.c.  The string must match AMIGA_URANDOM_DEV
 * there.  Read the entropy note in that file: the pool credits itself about
 * 21 bits and reports itself UNSEEDED.
 */
#define DROPBEAR_URANDOM_DEV "RANDOM:"

/*
 * The server halves sysoptions.h insists on having an answer for.
 *
 * DROPBEAR_SVR_PASSWORD_AUTH must be 0: `#error ... requires crypt()`.
 * DROPBEAR_SVR_MULTIUSER must be 0: DROPBEAR_SVR_DROP_PRIVS follows it and
 * needs setresgid().
 *
 * The second one is not free, and it is worth knowing where it lands: with it
 * at 0, common-session.c:71 arms a RUNTIME guard in every binary including the
 * client -- it calls getgroups(0, NULL) and exits unless the answer is -1 with
 * ENOSYS, so that this option cannot be set by accident on a machine that does
 * have users.  AmigaOS 3.x has neither users nor groups, so
 * clients/dropbear/amiga_dropbear.c answers ENOSYS and the guard passes for
 * the true reason rather than by being defeated.
 */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_MULTIUSER 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0

#endif
