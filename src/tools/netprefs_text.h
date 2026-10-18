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

/* Returns 1 and writes *newlen, or 0 when cap is too small.  Lines are
   matched as KEYWORD=value or KEYWORD value; written as KEYWORD = value. */
int np_text_patch(const char *old, size_t oldlen, NpTextField *fields,
                  size_t count, char *out, size_t cap, size_t *newlen);

/* The same, writing KEYWORD<sep>value: " " for name_resolution. */
int np_text_patch_sep(const char *old, size_t oldlen, NpTextField *fields,
                      size_t count, const char *sep, char *out, size_t cap,
                      size_t *newlen);

#define NP_TEXT_VALUE_LEN 48

/* The first word after each active line's key, in file order: at most max.
   Returns how many. */
size_t np_text_values(const char *text, size_t len, const char *key,
                      char values[][NP_TEXT_VALUE_LEN], size_t max);

/* Recognise one AddNetInterface line for name, or its all-files wildcard. */
int np_startup_line(const char *line, size_t len, const char *name,
                    int *commented, int *wildcard);

/* A name safe both as an interface filename and as an AmigaDOS argument. */
int np_interface_name_safe(const char *name, size_t limit);

#endif
