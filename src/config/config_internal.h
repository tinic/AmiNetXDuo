/*
 * AmiNetXDuo -- internal helpers for the configuration/netdb parsers.
 *
 * Split so that the text handling (config_text.c), the file-format parsers
 * (config_parse.c) and the netdb store (netdb.c) make no dos.library calls:
 * every byte they see arrives through the ami_cfg_read_file() hook implemented
 * in config_file.c. The host test (test/test_config.c) exercises the same code
 * with a stdio-backed hook.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CONFIG_INTERNAL_H
#define AMINETXDUO_CONFIG_INTERNAL_H

#include "aminetxduo/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ paths */

#define AMI_CFG_DIR_NETINTERFACES   "DEVS:NetInterfaces"
#define AMI_CFG_DIR_INTERNET        "DEVS:Internet"

#define AMI_CFG_FILE_NAMERES        AMI_CFG_DIR_INTERNET "/name_resolution"
#define AMI_CFG_FILE_GATEWAY        AMI_CFG_DIR_INTERNET "/default_gateway"
#define AMI_CFG_FILE_ROUTES         AMI_CFG_DIR_INTERNET "/routes"
#define AMI_CFG_FILE_HOSTS          AMI_CFG_DIR_INTERNET "/hosts"
#define AMI_CFG_FILE_NETWORKS       AMI_CFG_DIR_INTERNET "/networks"
#define AMI_CFG_FILE_PROTOCOLS      AMI_CFG_DIR_INTERNET "/protocols"
#define AMI_CFG_FILE_SERVICES       AMI_CFG_DIR_INTERNET "/services"

/*
 * Roadshow truncates the interface name (the config file's name) to 15
 * characters and ShowNetStatus displays that truncated form, so match it
 * rather than the 63 characters AMI_CFG_NAME_LEN would allow.
 */
#define AMI_CFG_IFNAME_MAX          15

/* A config file larger than this is assumed to be garbage. */
#define AMI_CFG_FILE_MAX            (256UL * 1024UL)

/* ------------------------------------------------------------- file access */

/*
 * Read a whole file into an ami_alloc()ed, NUL-terminated buffer. Returns NULL
 * if the file does not exist (which is never an error here) or on failure;
 * *size_out, when non-NULL, receives the byte count excluding the NUL.
 * The caller frees with ami_free().
 *
 * config_file.c has the dos.library implementation; the host test replaces it.
 */
APTR ami_cfg_read_file(const char *path, ULONG *size_out);

/* ------------------------------------------------------------ diagnostics */

/*
 * Name the file that subsequent ami_cfg_problem() calls are about. The
 * configuration is loaded one file at a time on one task, so a single current
 * file is enough and the parsers' signatures stay unchanged. Passing NULL
 * restores "the configuration".
 */
VOID ami_cfg_problem_file(const char *path);

/*
 * Hand one problem to whatever reporter is installed (usually none). `text`
 * says what is wrong, `hint` -- which may be NULL -- says what to do about it.
 * Both are copied nowhere: they may point at the caller's stack.
 */
VOID ami_cfg_problem(ULONG line, UWORD severity, const char *text,
                     const char *hint);

/* TRUE when a reporter is installed, so a caller can skip building a message. */
BOOL ami_cfg_problems_wanted(VOID);

/*
 * Build "<a><b><c>" (any of which may be NULL) into dst, for the one-off
 * message strings the parsers assemble. There is no sprintf() here.
 */
VOID ami_cfg_join3(char *dst, ULONG dstlen, const char *a, const char *b,
                   const char *c);

/* ----------------------------------------------------------- text handling */

/*
 * Pull the next line out of *cursor, NUL-terminating it in place. Handles LF,
 * CRLF and bare CR. Returns NULL once the buffer is exhausted. Blank lines are
 * returned as empty strings -- callers skip them.
 */
char *ami_cfg_next_line(char **cursor);

/* Trim leading and trailing whitespace in place; returns the new start. */
char *ami_cfg_trim(char *s);

/*
 * Truncate at the first unquoted comment character. `chars` lists the comment
 * introducers, e.g. "#;" or "#". Characters inside "double quotes" are exempt.
 */
VOID ami_cfg_strip_comment(char *s, const char *chars);

/*
 * Remove one layer of "double quotes", applying the AmigaDOS ReadItem escapes
 * (** -> *, *" -> ", *n -> LF, *e -> ESC). In place; a no-op when unquoted.
 */
VOID ami_cfg_unquote(char *s);

/*
 * Split a whitespace-separated line into tokens, NUL-terminating each in
 * place. Returns the token count (capped at max). Quoted tokens are unquoted.
 */
ULONG ami_cfg_tokenize(char *line, char **tokens, ULONG max);

/*
 * Pull the next `KEY=value` or `KEY value` pair off *cursor (which points into
 * a mutable line). Returns FALSE when the line is exhausted. Both outputs are
 * NUL-terminated in place; *value points at "" for a valueless keyword.
 */
BOOL ami_cfg_next_pair(char **cursor, char **key, char **value);

int  ami_cfg_stricmp(const char *a, const char *b);
ULONG ami_cfg_strlen(const char *s);
VOID ami_cfg_copy_string(char *dst, ULONG dstlen, const char *src);
VOID ami_cfg_zero(APTR p, ULONG len);

/* Decimal (or 0x.. / 0.. ) unsigned integer. FALSE on trailing garbage. */
BOOL ami_cfg_parse_ulong(const char *s, ULONG *out);

/* YES/NO/TRUE/FALSE/ON/OFF/1/0. */
BOOL ami_cfg_parse_bool(const char *s, BOOL *out);

/*
 * Network number in /etc/networks shorthand: 1..4 dot-separated parts, the
 * value left-aligned into the high octets ("127" -> 127.0.0.0).
 */
BOOL ami_cfg_parse_net_number(const char *s, ULONG *out);

/* --------------------------------------------------------- format parsers */

/*
 * Parse one DEVS:NetInterfaces file. `buf` is modified in place. `name` is the
 * file name, which becomes the interface name. Returns AMI_CFG_OK, or
 * AMI_CFG_ERR_SYNTAX when the mandatory DEVICE keyword is missing.
 */
LONG ami_cfg_parse_interface(const char *name, char *buf, AmiIfConfig *out);

/*
 * Parse DEVS:Internet/name_resolution into *out (which is added to, not
 * cleared). Also tolerates AmiTCP netdb-myhost lines: HOST/NAMESERVER/DOMAIN.
 * When `hostname` is non-NULL, a HOST line whose address is not a loopback
 * address fills it in (AmiTCP's "my host name" convention).
 */
VOID ami_cfg_parse_resolver(char *buf, AmiResolverConfig *out,
                            char *hostname, ULONG hostname_len);

/*
 * Parse DEVS:Internet/default_gateway (DEVICE/UNIT/GATEWAY) or
 * DEVS:Internet/routes (DEFAULT=/DEFAULTGATEWAY=, plus DST/VIA lines we skip).
 * Sets *out only when a default gateway is found.
 */
VOID ami_cfg_parse_gateway(char *buf, ULONG *out);

/*
 * Iterate the hosts table. Not in the public header, since the bsdsocket LVO
 * table has no gethostent(); ami_config_load() uses this to guess the local
 * host name.
 */
const AmiNetdbEntry *ami_netdb_host_entry(ULONG index);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_CONFIG_INTERNAL_H */
