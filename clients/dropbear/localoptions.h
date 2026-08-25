/* clients/dropbear/localoptions.h: the whole difference between upstream's
 * default_options.h and this build.  build.sh copies it into the BUILD
 * directory, which is where Dropbear reads it from.  SPDX-License-Identifier: MIT */

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

/* no data cache on this part, so a small table costs the same per lookup as a
   large one and small-code layouts only pay rotates: a pessimisation here */
#define DROPBEAR_SMALL_CODE 0

/* The optimistic kex guess sends a KEXDH_INIT before the server's KEXINIT, on
   the bet that the server prefers our first choice.  A modern OpenSSH prefers an
   ML-KEM hybrid, off above, so the bet always loses and costs a scalar multiply. */
#define DROPBEAR_KEX_FIRST_FOLLOWS 0

/* no ~/.ssh/config to read, and no MOTD worth printing over a serial-speed
   link */
#define DROPBEAR_USE_SSH_CONFIG 0
#define DO_MOTD 0

/* dbrandom.c's seedrandom() has no fallback and there is no build without a
   device.  This string must match AMIGA_URANDOM_DEV in clients/dropbear/
   amiga_dropbear.c, which intercepts open() for exactly it. */
#define DROPBEAR_URANDOM_DEV "RANDOM:"

/* sysoptions.h insists on an answer.  PASSWORD_AUTH must be 0 (`#error ...
   requires crypt()`); MULTIUSER must be 0 (DROP_PRIVS needs setresgid()), and at
   0 it arms common-session.c:71's getgroups()-must-answer-ENOSYS guard. */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_MULTIUSER 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0

#endif
