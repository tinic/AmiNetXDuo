/* WebDAV filesystem policy shared by httpd's method handlers.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPFS_H
#define AMINETXDUO_HTTPFS_H

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

ULONG http_fs_status(LONG err);
BOOL  http_fs_parent(const char *path, char *out, ULONG outlen);
BOOL  http_fs_name_differs(const struct FileInfoBlock *fib,
                           const char *name);
BOOL  http_fs_name_cut(struct FileInfoBlock *scratch, const char *path,
                       const char *name);
BOOL  http_fs_name_survives(struct FileInfoBlock *scratch, const char *path,
                            const char *name);
BOOL  http_fs_entry_is_link(LONG type);
VOID  http_fs_etag(ULONG size, const struct DateStamp *ds,
                   char *out, ULONG outlen);
VOID  http_fs_etag_of(struct FileInfoBlock *scratch, const char *path,
                      char *out, ULONG outlen);
LONG  http_fs_kind(struct FileInfoBlock *scratch, const char *path);
ULONG http_fs_free_bytes(struct InfoData *scratch, const char *path);

#endif /* AMINETXDUO_HTTPFS_H */
