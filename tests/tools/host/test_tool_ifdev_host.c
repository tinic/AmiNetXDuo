/*
 * tool_ifdev.c: the whole device path from NETSTATUS_IFDEVICES, and
 * nsi_Device from a library that predates it.  The library half is
 * tests/bsdsocket/host/test_ifdevices_host.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr, message)                                                \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            fprintf(stderr, "FAIL: %s\n", message);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* 40 characters, where nsi_Device keeps 31. */
static const char long_path[] = "Workbench:Devs/Networks/x-surf-100.device";

/* What the mocked library answers. */
static LONG               q_rc;
static UWORD              q_count;
static NetStatusIfDevice  q_e[8];
static unsigned           q_calls;

LONG tool_netstatus_query(struct Library *base, ULONG what,
                          APTR buffer, ULONG size, ULONG entry_size)
{
    NetStatusHeader *hdr = (NetStatusHeader *)buffer;
    ULONG            room;
    UWORD            n;

    (void)base;
    q_calls++;
    CHECK(what == NETSTATUS_IFDEVICES, "the selector asked is IFDEVICES");
    CHECK(entry_size == sizeof(NetStatusIfDevice), "at the published size");

    if (q_rc < 0)
        return -1;                      /* EINVAL: an older library */

    room = (size - (ULONG)sizeof(NetStatusHeader)) / entry_size;
    n    = (q_count < room) ? q_count : (UWORD)room;
    memcpy(NETSTATUS_ENTRIES(hdr), q_e, n * sizeof(NetStatusIfDevice));
    hdr->nsh_Count = n;
    return n;
}

VOID tool_copy_string(char *dst, ULONG dstlen, const char *src)
{
    ULONG i;

    for (i = 0; i + 1 < dstlen && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

int tool_stricmp_n(const char *a, const char *b, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
    {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        int cb = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i];

        if (ca != cb)
            return ca - cb;
        if (ca == '\0')
            return 0;
    }
    return 0;
}

int tool_stricmp(const char *a, const char *b)
{
    return tool_stricmp_n(a, b, 0xFFFFFFFFUL);
}

static VOID stage(UWORD at, UWORD index, const char *device)
{
    memset(&q_e[at], 0, sizeof(q_e[at]));
    q_e[at].nsd_Index = index;
    tool_copy_string(q_e[at].nsd_Device, NETSTATUS_FILE_LEN, device);
}

/* An interface record as the library cuts it: the first 31 characters. */
static VOID live(NetStatusInterface *e, UWORD index, const char *device)
{
    memset(e, 0, sizeof(*e));
    e->nsi_Index = index;
    tool_copy_string(e->nsi_Device, NETSTATUS_DEVICE_LEN, device);
}

static VOID whole_path_wins(VOID)
{
    NetStatusInterface e;
    char               out[NETSTATUS_FILE_LEN];

    q_rc = 0; q_count = 2; q_calls = 0;
    stage(0, 0, "a2065.device");
    stage(1, 1, long_path);
    tool_netstatus_devices(NULL);
    CHECK(q_calls == 1, "one query for every interface");

    live(&e, 1, long_path);
    tool_if_device(out, sizeof(out), &e);
    CHECK(strcmp(out, long_path) == 0, "a path over 31 characters arrives whole");

    live(&e, 0, "a2065.device");
    tool_if_device(out, sizeof(out), &e);
    CHECK(strcmp(out, "a2065.device") == 0, "a short one is unchanged");
}

static VOID old_library_keeps_nsi_device(VOID)
{
    NetStatusInterface e;
    char               out[NETSTATUS_FILE_LEN];

    q_rc = 0; q_count = 1;
    stage(0, 1, long_path);
    tool_netstatus_devices(NULL);       /* a current answer first ...     */

    q_rc = -1;
    tool_netstatus_devices(NULL);       /* ... then an old library's      */

    live(&e, 1, long_path);
    tool_if_device(out, sizeof(out), &e);
    CHECK(strlen(out) == NETSTATUS_DEVICE_LEN - 1 &&
          strncmp(out, long_path, NETSTATUS_DEVICE_LEN - 1) == 0,
          "EINVAL keeps nsi_Device, not an earlier answer");
}

static VOID empty_or_foreign_slot_falls_back(VOID)
{
    NetStatusInterface e;
    char               out[NETSTATUS_FILE_LEN];

    q_rc = 0; q_count = 2;
    stage(0, 0, "");                    /* attached, no configured path */
    stage(1, 9, long_path);             /* an index no interface has    */
    tool_netstatus_devices(NULL);

    live(&e, 0, "ram.device");
    tool_if_device(out, sizeof(out), &e);
    CHECK(strcmp(out, "ram.device") == 0, "an empty slot keeps nsi_Device");

    live(&e, 1, "x-surf.device");
    tool_if_device(out, sizeof(out), &e);
    CHECK(strcmp(out, "x-surf.device") == 0,
          "an out-of-range nsd_Index is matched to nothing");
}

static VOID unterminated_fields_are_bounded(VOID)
{
    NetStatusInterface e;
    char               out[NETSTATUS_FILE_LEN];

    q_rc = 0; q_count = 1;
    stage(0, 2, "");
    memset(q_e[0].nsd_Device, 'x', NETSTATUS_FILE_LEN);
    tool_netstatus_devices(NULL);

    live(&e, 2, "");
    tool_if_device(out, sizeof(out), &e);
    CHECK(strlen(out) == NETSTATUS_FILE_LEN - 1, "nsd_Device is cut at its field");

    q_rc = -1;
    tool_netstatus_devices(NULL);
    memset(e.nsi_Device, 'y', NETSTATUS_DEVICE_LEN);
    e.nsi_Unit = 0x7A7A7A7AUL;          /* "zzzz", the field after it */
    tool_if_device(out, sizeof(out), &e);
    CHECK(strlen(out) == NETSTATUS_DEVICE_LEN && out[0] == 'y' &&
          out[NETSTATUS_DEVICE_LEN - 1] == 'y',
          "nsi_Device is read to its own field and no further");
}

/*
 * A path that is exactly 31 characters and arrived whole is not a cut copy:
 * a longer DEVICE= that starts with it is another driver (codex-amiga's
 * review of 31c39ce4).  From an old library the same 31 characters may be
 * the cut, and the prefix rule stands.
 */
static VOID exact_match_when_whole(VOID)
{
    static const char p31[]  = "Work:Devs/Networks/abcdefgh.dev";
    static const char p35[]  = "Work:Devs/Networks/abcdefgh.device";
    NetStatusInterface e;
    char               out[NETSTATUS_FILE_LEN];
    BOOL               whole;

    CHECK(strlen(p31) == NETSTATUS_DEVICE_LEN - 1, "the fixture is 31 long");

    q_rc = 1; q_count = 1;
    stage(0, 0, p31);
    tool_netstatus_devices(NULL);
    live(&e, 0, p31);
    whole = tool_if_device(out, sizeof(out), &e);
    CHECK(whole, "a slot the library answered is whole");
    CHECK(!tool_device_matches(p35, out, whole),
          "a whole 31-character path is not a longer file's prefix");
    CHECK(tool_device_matches("WORK:devs/networks/ABCDEFGH.dev", out, whole),
          "a whole path still matches without regard to case");

    q_rc = -1;
    tool_netstatus_devices(NULL);
    live(&e, 0, p35);
    whole = tool_if_device(out, sizeof(out), &e);
    CHECK(!whole, "an old library's nsi_Device is not whole");
    CHECK(tool_device_matches(p35, out, whole),
          "an old library's 31-character copy matches the file it was cut from");
    CHECK(!tool_device_matches("Work:Devs/Networks/abcdefgX.device", out, whole),
          "and not a file that differs inside those 31");

    live(&e, 0, "a2065.device");
    whole = tool_if_device(out, sizeof(out), &e);
    CHECK(!tool_device_matches("a2065.device.old", out, whole),
          "a short copy is compared exactly");
}

int main(void)
{
    whole_path_wins();
    old_library_keeps_nsi_device();
    empty_or_foreign_slot_falls_back();
    unterminated_fields_are_bounded();
    exact_match_when_whole();

    printf("tool_ifdev failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
