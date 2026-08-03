/*
 * AmiNetXDuo -- usergroup.library: the passwd and group file parsers.
 *
 * ug_db.c reads the files and calls these; the host test and the fuzz driver
 * call them directly with bytes of their own.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UG_PARSE_H
#define AMINETXDUO_UG_PARSE_H

#include "usergroup_internal.h"

/* Both parse in place: text is written through and the records point into it. */
void ug_db_parse_passwd(struct UgDatabase *db, char *text);
void ug_db_parse_group(struct UgDatabase *db, char *text, ULONG len);

/* The single-user, single-group tables every machine without the files gets. */
void ug_db_default_passwd(struct UgDatabase *db);
void ug_db_default_group(struct UgDatabase *db);

#endif /* AMINETXDUO_UG_PARSE_H */
