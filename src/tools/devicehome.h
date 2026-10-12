/* Where an interface file's DEVICE= finds one of this stack's drivers.  Pure
   text, used by InstallSameFile and tested on the host (test_devicehome). */
#ifndef AMINETXDUO_DEVICEHOME_H
#define AMINETXDUO_DEVICEHOME_H

#include <stddef.h>

/* The drawer layout's own name for its drivers. */
#define DH_ASSIGN_NETWORKS "AmiNetXDuo:Devs/Networks"

/* For a DEVICE value naming one of OUR drivers by an absolute path that does
   not go through the AmiNetXDuo: assign: returns 1, the directory part in
   dir (NUL-terminated; "Vol:" or "Vol:a/b") and the driver's own name.
   Returns 0 for anything else, including a dir that does not fit. */
int dh_absolute_driver(const char *value, size_t len, char *dir,
                       size_t dir_cap, const char **driver);

/* Asked for each candidate directory: nonzero when it is this drawer's. */
typedef int (*DhOursFn)(const char *dir, void *ctx);

/* Copy old to out, replacing the value of every DEVICE line that
   dh_absolute_driver() accepts and ours() claims by
   AmiNetXDuo:Devs/Networks/<driver>.  Every other byte is kept.  Returns the
   number of lines rewritten, or -1 when out is too small. */
long dh_rehome(const char *old, size_t oldlen, char *out, size_t cap,
               size_t *newlen, DhOursFn ours, void *ctx);

#endif
