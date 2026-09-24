/*
 * CreateAmiNetXDuoStatusReport, the half that decides what a line says.
 *
 * Every line is `key=value`.  Nothing here calls AmigaOS, so the host test
 * (test/test_statusreport.c) runs the same code the command does.
 *
 * WHAT MAY BE PRINTED IS AN ALLOWLIST, built up key by key.  An interface file
 * is never copied out and scrubbed: each keyword is looked up in sr_if_keys[],
 * a keyword not there is counted and dropped, and a keyword that is there has
 * its value checked against the shape that keyword takes.  Addresses, names
 * and station addresses are a second class that prints only when the caller
 * set SrOut.addresses.  A password, a key or a token cannot appear, because no
 * row admits one.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_STATUSREPORT_TEXT_H
#define AMINETXDUO_STATUSREPORT_TEXT_H

#include <exec/types.h>

#define SR_UNAVAILABLE  "unavailable"
#define SR_INVALID      "invalid"

#define SR_KEY_MAX      96      /* a whole key, dots included               */
#define SR_VALUE_MAX    96      /* a value is cut here                      */
#define SR_LINE_MAX     (SR_KEY_MAX + SR_VALUE_MAX + 4)

/* One finished line, "key=value\n", to wherever the report goes. */
typedef VOID (*SrWrite)(APTR user, const char *line);

typedef struct SrOut
{
    SrWrite write;
    APTR    user;
    BOOL    addresses;          /* ADDRESSES: the second class may print    */
    ULONG   lines;              /* written so far                            */
} SrOut;

/*
 * The console and the file, one line to each.  `put` answers 0 when the line
 * went out.  The first file line that does not stops the file and is kept in
 * file_failed; the console still gets every line.
 */
typedef LONG (*SrPut)(APTR handle, const char *line);

typedef struct SrTee
{
    SrPut   put;
    APTR    console;
    APTR    file;               /* NULL: no file                            */
    BOOL    file_failed;
} SrTee;

/* An SrWrite; `user` is the SrTee. */
VOID sr_tee_write(APTR user, const char *line);

/* ------------------------------------------------------------ one line --- */

/* NULL is SR_UNAVAILABLE.  Control characters in `value` become '?'. */
VOID sr_str(SrOut *o, const char *key, const char *value);
VOID sr_ulong(SrOut *o, const char *key, ULONG value);
VOID sr_long(SrOut *o, const char *key, LONG value);
VOID sr_hex(SrOut *o, const char *key, ULONG value);           /* $0000002a */
VOID sr_yesno(SrOut *o, const char *key, BOOL value);
VOID sr_version(SrOut *o, const char *key, ULONG version, ULONG revision);

/* The second class.  Nothing at all is printed without SrOut.addresses. */
VOID sr_addr_str(SrOut *o, const char *key, const char *value);
VOID sr_addr_ipv4(SrOut *o, const char *key, ULONG addr);      /* host order */
VOID sr_addr_mac(SrOut *o, const char *key, const UBYTE *mac);  /* six bytes */

/* ------------------------------------------------------------- keys --- */

/*
 * "a.b.c" into `dst`, any part NULL to stop early.  A part that is not ours --
 * an interface file's name, a device name -- goes through sr_key_part(), so
 * nothing in it can be read as a second key or a second line.
 */
VOID sr_key(char *dst, ULONG dstlen, const char *a, const char *b,
            const char *c);
VOID sr_key_part(char *dst, ULONG dstlen, const char *text);
VOID sr_key_index(char *dst, ULONG dstlen, const char *prefix, ULONG index,
                  const char *suffix);

/* Appending to a NUL-terminated value, cut at dstlen.  $hex is eight digits. */
VOID sr_cat(char *dst, ULONG dstlen, const char *text);
VOID sr_cat_ulong(char *dst, ULONG dstlen, ULONG value);
VOID sr_cat_long(char *dst, ULONG dstlen, LONG value);
VOID sr_cat_hex(char *dst, ULONG dstlen, ULONG value);

/* ------------------------------------------------------------ decoding --- */

/* The processor and the FPU, from ExecBase->AttnFlags.  Never NULL. */
const char *sr_cpu_name(ULONG attn);
const char *sr_fpu_name(ULONG attn);

/* ------------------------------------------------------ interface files --- */

/*
 * One DEVS:NetInterfaces file, already in memory and NUL-terminated.  Parsed
 * IN PLACE with the configuration layer's own tokenizer, so a key this sees is
 * the key the stack sees.  Prints config.<name>.<keyword>=<value> for every
 * allowlisted keyword, then config.<name>.omitted=<count of the rest>.
 *
 * `device` (may be NULL) receives the DEVICE= driver's file name, without a
 * path, when the value was well formed.  Returns the lines printed.
 */
ULONG sr_interface_text(SrOut *o, const char *ifname, char *text,
                        char *device, ULONG devicelen);

/* ------------------------------------------------------ the probe record --- */

/*
 * Whether one step of an anx driver's probe record may print its value.
 *   1  yes
 *   0  only with SrOut.addresses: it carries station-address or serial bytes
 *  -1  a code this build does not know; print SR_UNAVAILABLE for it
 */
LONG sr_anxdiag_value_class(UWORD code);

#endif /* AMINETXDUO_STATUSREPORT_TEXT_H */
