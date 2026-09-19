/* Pure text editing used by NetPrefs and its host regression test. */
#ifndef AMINETXDUO_NETPREFS_TEXT_H
#define AMINETXDUO_NETPREFS_TEXT_H

#include <stddef.h>

typedef struct NpTextField
{
    const char *key;
    const char *value;                 /* NULL removes the key */
    int         seen;
} NpTextField;

/* Returns 1 and writes *newlen, or 0 when cap is too small. */
int np_text_patch(const char *old, size_t oldlen, NpTextField *fields,
                  size_t count, char *out, size_t cap, size_t *newlen);

/* Recognise one AddNetInterface line for name, or its all-files wildcard. */
int np_startup_line(const char *line, size_t len, const char *name,
                    int *commented, int *wildcard);

/* A name safe both as an interface filename and as an AmigaDOS argument. */
int np_interface_name_safe(const char *name, size_t limit);

#endif
