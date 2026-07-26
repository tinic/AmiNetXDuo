/*
 * clients/dropbear/localoptions-p256.h -- the same client with the OTHER half
 * of its algorithm set: P-256 and RSA instead of curve25519 and ed25519.
 *
 * WHAT THIS IS FOR
 *
 *   docs/RESEARCH.md 31.6 asked one question and could not answer it: "is
 *   P-256 faster on this machine than curve25519?"  It is not a rhetorical
 *   question.  Dropbear's curve25519 and ed25519 are TweetNaCl -- a field
 *   element is sixteen 16-bit limbs stored in an i64[16], and a field multiply
 *   is 256 software 64x64 multiplies on a machine that has a 32x32->64 in
 *   hardware.  Dropbear's P-256 is libtomcrypt over libtommath, which is
 *   ordinary C but is at least using the machine's multiplier.  Neither is
 *   src/crypto68k/, and that is the point: this build changes NO
 *   implementation, only which one the protocol picks.
 *
 *   It is built from clients/dropbear/build.sh -O this-file, so the ONLY
 *   difference from the shipping client is the list below.
 *
 * WHY ED25519 HAS TO GO TOO, AND NOT ONLY CURVE25519
 *
 *   Turning off DROPBEAR_CURVE25519 moves the KEY EXCHANGE to
 *   ecdh-sha2-nistp256 and nothing else.  The server's HOST KEY signature and
 *   the client's own authentication signature are chosen by the hostkey and
 *   publickey algorithm lists, and with ed25519 still compiled in the client
 *   asks for ssh-ed25519 and keeps paying TweetNaCl for both.  So this file
 *   moves the whole suite or none of it, and the client key handed to -i has
 *   to be an ECDSA one -- clients/dropbear/sshd-testserver.sh makes
 *   build/sshd-test/id_amiga_ecdsa alongside the ed25519 one, and installs an
 *   ECDSA host key for the server to answer with.
 *
 * WHAT IS **NOT** CHANGED, DELIBERATELY
 *
 *   The cipher.  31.5 measured aes128-ctr against chacha20-poly1305 on the
 *   same connection and they differed by 0.14 s out of 96, so the record path
 *   is not a variable here and pinning it would only add a difference that
 *   does not matter.
 *
 * Everything below this comment that is not a 25519 switch is copied from
 * clients/dropbear/localoptions.h and means the same thing there; read that
 * file for why.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DROPBEAR_AMIGA_LOCALOPTIONS_P256_H
#define DROPBEAR_AMIGA_LOCALOPTIONS_P256_H

/* ------------------------------------------------ THE WHOLE EXPERIMENT --- */

#define DROPBEAR_CURVE25519 0       /* kex     -> ecdh-sha2-nistp256      */
#define DROPBEAR_ED25519 0          /* hostkey -> ecdsa-sha2-nistp256     */
                                    /* pubkey  -> ecdsa-sha2-nistp256     */

/* ------------------------------------ everything else: as the shipping ---
   client, so that the two binaries differ in nothing but the three lines
   above and the objects they drag in. */

#define DROPBEAR_CLI_PROXYCMD 0
#define DROPBEAR_CLI_NETCAT 0
#define DROPBEAR_CLI_ASKPASS_HELPER 0

#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0

#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_SK_KEYS 0
#define DROPBEAR_DSS 0
#define DROPBEAR_X11FWD 0

#define DROPBEAR_SMALL_CODE 0

#define DROPBEAR_KEX_FIRST_FOLLOWS 0

#define DROPBEAR_USE_SSH_CONFIG 0
#define DO_MOTD 0

#define DROPBEAR_URANDOM_DEV "RANDOM:"

#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_MULTIUSER 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0

#endif
