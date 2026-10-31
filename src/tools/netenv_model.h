/* Configuration-only ENV exports. No AmigaOS calls or network traffic.
 * SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_NETENV_MODEL_H
#define AMINETXDUO_NETENV_MODEL_H
#include <stddef.h>
#include <stdint.h>
#define NE_IF_MAX 32
#define NE_ADDR6_MAX 128
#define NE_ROUTE_MAX 96
#define NE_KEY_MAX (NE_IF_MAX * 4 + 9)
#define NE_TEXT_MAX 12288
#define NE_VALUE_MAX 8192

typedef struct NeInterface {
    char name[16];
    uint16_t index;
    int up;
    uint32_t address, mask;
} NeInterface;
typedef struct NeAddress6 {
    char text[48];
    uint16_t index;
    unsigned prefix;
    int ready, scoped;
} NeAddress6;
typedef struct NeRoute {
    char gateway[48];
    uint16_t index;
    int ipv6, scoped;
} NeRoute;
typedef struct NeSnapshot {
    const char *state; /* UP, DOWN, ERROR */
    unsigned interfaces, addresses6, routes;
    NeInterface iface[NE_IF_MAX];
    NeAddress6 addr6[NE_ADDR6_MAX];
    NeRoute route[NE_ROUTE_MAX];
} NeSnapshot;
typedef struct NeEntry {
    char name[32];
    unsigned offset;
} NeEntry;
typedef struct NeTable {
    unsigned count, used;
    NeEntry entry[NE_KEY_MAX];
    char text[NE_TEXT_MAX];
} NeTable;
typedef struct NeOwned {
    unsigned count;
    char name[NE_KEY_MAX][32];
} NeOwned;
/* read: 1 = found, 0 = absent, -1 = unreadable; write: nonzero = success.
 * NULL value deletes. Only successful deletions relinquish ownership. */
typedef int (*NeRead)(void *, const char *, char *, size_t);
typedef int (*NeWrite)(void *, const char *, const char *);
int ne_name_valid(const char *name);
int ne_build(const NeSnapshot *snapshot, NeTable *out);
const char *ne_value(const NeTable *table, const char *name);
int ne_own(NeOwned *owned, const char *name);
int ne_publish(NeOwned *owned, const NeTable *desired,
               NeRead read_value, NeWrite write_value, void *ctx);
/* AmigaDOS IoErr() values for an ENV: object that is already gone. */
#define NE_ERR_DIR_NOT_FOUND 204
#define NE_ERR_OBJECT_NOT_FOUND 205
/* DeleteFile() of an ENV: path: 0 = deleted, else its IoErr(). */
typedef long (*NeDelete)(void *, const char *);
/* Delete ENV:<name>, then its directories bottom-up while they are empty.
 * Never ENV: itself. A missing variable or directory counts as deleted. */
int ne_remove(const char *name, NeDelete delete_path, void *ctx);
/* One refresh through bsdsocket.library. open: base or NULL; collect: 0 =
 * failed; pending: CTRL-C set, cleared when clear is nonzero. NetShutdown
 * sends CTRL-C to every opener: one that arrives while the base is open
 * means "let go", is taken, and leaves the stack DOWN. One already set
 * before open is a user break and is left for the caller's Wait(). A user
 * break inside the refresh reads as the first case; Break again. */
typedef struct NeStack {
    void *(*open)(void *ctx);
    int (*collect)(void *ctx, void *base, NeSnapshot *s);
    void (*close)(void *ctx, void *base);
    int (*pending)(void *ctx, int clear);
    void *ctx;
} NeStack;
void ne_refresh(NeSnapshot *s, const NeStack *stack);
/* The $VER: string without its "$VER: " prefix. */
const char *ne_version(const char *verstag);
#endif
