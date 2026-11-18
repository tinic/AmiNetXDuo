/*
 * updkeys, the keys this build of UpdateAmiNetXDuo accepts; see updkeys.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UPDKEYS_H
#define AMINETXDUO_UPDKEYS_H

#include "updsig.h"

extern const UpdKey       upd_keys[];
extern const unsigned int upd_key_count;    /* 0: refuse every manifest  */

#endif /* AMINETXDUO_UPDKEYS_H */
