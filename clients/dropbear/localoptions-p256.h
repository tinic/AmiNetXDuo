/* clients/dropbear/localoptions-p256.h: the same client with P-256 and RSA
 * instead of curve25519 and ed25519 (build.sh -O this-file).  Both 25519 switches
 * must move together, and -i then needs an ECDSA key.  SPDX-License-Identifier: MIT */

#ifndef DROPBEAR_AMIGA_LOCALOPTIONS_P256_H
#define DROPBEAR_AMIGA_LOCALOPTIONS_P256_H

/* ---------------------------------------------------- the experiment ----- */

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
