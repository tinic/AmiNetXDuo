/*
 * CreateAmiNetXDuoStatusReport: what it may print, and what it may call.
 *
 *   allowlist   statusreport_text.c, run: an interface file full of secrets
 *               and addresses gives up neither without ADDRESSES, and with
 *               it gives up the addresses and still no secret
 *   passive     the command's sources, read: nothing in them opens a
 *               device, loads a library, or reaches the openers that do
 *
 * No argument runs both.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "statusreport_text.h"
#include "aminetxduo/anxdiag.h"

#ifndef AMINETXDUO_SOURCE_DIR
#define AMINETXDUO_SOURCE_DIR "."
#endif

static const char *source_dir(void)
{
    const char *env = getenv("AMINETXDUO_SOURCE_DIR");

    return (env != NULL && env[0] != '\0') ? env : AMINETXDUO_SOURCE_DIR;
}

static int failures;
static int checks;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------ the sink --- */

static char out[16384];

static void capture(APTR user, const char *line)
{
    (void)user;
    if (strlen(out) + strlen(line) < sizeof(out))
        strcat(out, line);
}

static SrOut sink(int addresses)
{
    SrOut o;

    out[0]      = '\0';
    o.write     = capture;
    o.user      = NULL;
    o.addresses = (BOOL)addresses;
    o.lines     = 0;
    return o;
}

static int has(const char *text)
{
    return strstr(out, text) != NULL;
}

/* ------------------------------------------------------------ allowlist --- */

/* Every secret-shaped thing a file could carry, beside a real configuration.
   Three pairs share a line with an allowed one, as a tokenizer that took the
   rest of the line for a value would get wrong. */
static const char fake_file[] =
    "# written by hand\n"
    "DEVICE=DEVS:Networks/anxnet.device UNIT=0\n"
    "CONFIGURE=DHCP\n"
    "IPADDRESS=192.168.77.23 NETMASK=255.255.255.0 GATEWAY=192.168.77.1\n"
    "HARDWAREADDRESS=02:11:22:33:44:55\n"
    "PASSWORD=hunter2secret\n"
    "KEY=\"wpa psk s3cr3tphrase\"\n"
    "WPA_PSK correcthorsebattery\n"
    "TOKEN = tok_ABCDEF123\n"
    "ID=amiga-of-somebody\n"
    "NAMESERVER 9.9.9.9\n"
    "MTU=1500 SECRET=dontshowthis PSK=psk_value_here\n"
    "PRIORITY=hunterpriority\n"
    "STATE=up ; LOGIN=bob\n";

static const char *const secrets[] = {
    "hunter", "s3cr3t", "correcthorse", "tok_ABC", "dontshow", "psk_value",
    "bob", NULL
};

static const char *const identifying[] = {
    "192.168.77", "255.255.255.0", "02:11:22", "amiga-of-somebody",
    "9.9.9.9", NULL
};

static void run_file(int addresses, char *device, size_t devlen)
{
    char  text[sizeof(fake_file)];
    SrOut o = sink(addresses);

    memcpy(text, fake_file, sizeof(fake_file));
    (void)sr_interface_text(&o, "eth0", text, device, (ULONG)devlen);
}

static void check_allowlist(void)
{
    char device[40];
    int  i;

    /* ---- without ADDRESSES */
    run_file(0, device, sizeof(device));

    CHECK(has("config.eth0.device=DEVS:Networks/anxnet.device\n"),
          "the driver is reported: %s", out);
    CHECK(strcmp(device, "anxnet.device") == 0,
          "the driver's name is handed back without its path: '%s'", device);
    CHECK(has("config.eth0.unit=0\n"), "UNIT is reported");
    CHECK(has("config.eth0.configure=dhcp\n"), "CONFIGURE is the table's word");
    CHECK(has("config.eth0.mtu=1500\n"), "MTU shares a line with a secret");
    CHECK(has("config.eth0.state=up\n"), "STATE before a comment");
    CHECK(has("config.eth0.priority=invalid\n"),
          "a value of the wrong shape is printed as invalid, not as itself");
    CHECK(has("config.eth0.omitted=6\n"),
          "six keywords are not on the list: %s", out);

    for (i = 0; secrets[i] != NULL; i++)
        CHECK(!has(secrets[i]), "no secret without ADDRESSES: '%s' in\n%s",
              secrets[i], out);
    for (i = 0; identifying[i] != NULL; i++)
        CHECK(!has(identifying[i]), "no address without ADDRESSES: '%s' in\n%s",
              identifying[i], out);
    CHECK(!has(".address=") && !has(".gateway=") && !has(".id="),
          "no address key at all without ADDRESSES");

    /* ---- with ADDRESSES */
    run_file(1, device, sizeof(device));

    CHECK(has("config.eth0.address=192.168.77.23\n"), "ADDRESSES: the address");
    CHECK(has("config.eth0.netmask=255.255.255.0\n"), "ADDRESSES: the netmask");
    CHECK(has("config.eth0.gateway=192.168.77.1\n"), "ADDRESSES: the gateway");
    CHECK(has("config.eth0.hardwareaddress=02:11:22:33:44:55\n"),
          "ADDRESSES: the station address");
    CHECK(has("config.eth0.id=amiga-of-somebody\n"), "ADDRESSES: the name");
    CHECK(has("config.eth0.nameserver=9.9.9.9\n"), "ADDRESSES: the name server");
    CHECK(has("config.eth0.omitted=6\n"), "the same six are left out");

    for (i = 0; secrets[i] != NULL; i++)
        CHECK(!has(secrets[i]), "no secret with ADDRESSES either: '%s' in\n%s",
              secrets[i], out);
}

static void check_lines(void)
{
    SrOut o;
    char  key[SR_KEY_MAX];
    UBYTE mac[6] = { 2, 0x11, 0x22, 0x33, 0x44, 0x55 };

    /* unavailable is a value, printed as such */
    o = sink(0);
    sr_str(&o, "stack.profile", NULL);
    CHECK(strcmp(out, "stack.profile=unavailable\n") == 0,
          "NULL prints as unavailable: '%s'", out);

    o = sink(0);
    (void)sr_interface_text(&o, "eth1", NULL, NULL, 0);
    CHECK(strcmp(out, "config.eth1.file=unavailable\n") == 0,
          "an unreadable file is unavailable: '%s'", out);

    /* the second class is silent without the switch, not "unavailable" */
    o = sink(0);
    sr_addr_ipv4(&o, "live.eth0.address", 0xc0a84d17UL);
    sr_addr_mac(&o, "live.eth0.hardwareaddress", mac);
    sr_addr_str(&o, "stack.hostname", "amiga");
    CHECK(out[0] == '\0' && o.lines == 0, "no address line without ADDRESSES: '%s'", out);

    o = sink(1);
    sr_addr_ipv4(&o, "live.eth0.address", 0xc0a84d17UL);
    sr_addr_mac(&o, "live.eth0.hardwareaddress", mac);
    CHECK(has("live.eth0.address=192.168.77.23\n"), "dotted quad: %s", out);
    CHECK(has("live.eth0.hardwareaddress=02:11:22:33:44:55\n"), "colon MAC: %s", out);

    /* one line stays one line */
    o = sink(0);
    sr_str(&o, "a", "x\ny=1\r");
    CHECK(strcmp(out, "a=x?y=1?\n") == 0, "control characters: '%s'", out);

    sr_key_part(key, sizeof(key), "eth0\nreport.end=complete");
    CHECK(strcmp(key, "eth0_report.end_complete") == 0,
          "a file name cannot make a second key: '%s'", key);

    o = sink(0);
    sr_long(&o, "n", -2147483647L - 1L);
    sr_version(&o, "v", 40, 68);
    sr_hex(&o, "h", 0x2aUL);
    CHECK(strcmp(out, "n=-2147483648\nv=40.68\nh=$0000002a\n") == 0,
          "numbers: '%s'", out);
}

static void check_decoding(void)
{
    CHECK(strcmp(sr_cpu_name(0), "68000") == 0, "no bits is a 68000");
    CHECK(strcmp(sr_fpu_name(0), "none") == 0, "no bits is no FPU");
    CHECK(strcmp(sr_cpu_name(0x07), "68030") == 0, "68030");
    CHECK(strcmp(sr_fpu_name(0x07 | 0x30), "68882") == 0, "68882");
    CHECK(strcmp(sr_cpu_name(0x0f | 0x70), "68040") == 0, "68040");
    CHECK(strcmp(sr_fpu_name(0x0f | 0x70), "68040") == 0, "68040 FPU");
    CHECK(strcmp(sr_cpu_name(0x8f | 0x70), "68060") == 0, "68060");
    CHECK(strcmp(sr_fpu_name(0x8f | 0x70), "68060") == 0, "68060 FPU");
    CHECK(strcmp(sr_cpu_name(0x8f), "68060") == 0 &&
          strcmp(sr_fpu_name(0x8f), "none") == 0, "68LC060");

    CHECK(sr_anxdiag_value_class(ANXDIAG_MAC_HI) == 0, "MAC_HI is identifying");
    CHECK(sr_anxdiag_value_class(ANXDIAG_MAC_LO) == 0, "MAC_LO is identifying");
    CHECK(sr_anxdiag_value_class(ANXDIAG_PNP_SERIAL) == 0, "a serial number");
    CHECK(sr_anxdiag_value_class(ANXDIAG_NE_NODEID_PORT) == 0, "node ID bytes");
    CHECK(sr_anxdiag_value_class(ANXDIAG_CHIP) == 1, "the chip is plain");
    CHECK(sr_anxdiag_value_class(ANXDIAG_ATTACH_FAIL) == 1, "a refusal is plain");
    CHECK(sr_anxdiag_value_class(250) == -1, "an unknown code prints nothing");
}

/* -------------------------------------------------------------- passive --- */

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long  n;
    char *buf;

    if (fp == NULL)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    rewind(fp);
    buf = malloc((size_t)n + 1);
    if (buf == NULL) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

/* Comments and string literals out, so a sentence that NAMES a call is not
   read as the call. */
static void code_only(char *s)
{
    char *r = s;
    char *w = s;

    while (*r != '\0') {
        if (r[0] == '/' && r[1] == '*') {
            r += 2;
            while (*r != '\0' && !(r[0] == '*' && r[1] == '/'))
                r++;
            if (*r != '\0')
                r += 2;
            *w++ = ' ';
            continue;
        }
        if (r[0] == '/' && r[1] == '/') {
            while (*r != '\0' && *r != '\n')
                r++;
            continue;
        }
        if (*r == '"' || *r == '\'') {
            char q = *r++;

            while (*r != '\0' && *r != q) {
                if (*r == '\\' && r[1] != '\0')
                    r++;
                r++;
            }
            if (*r != '\0')
                r++;
            *w++ = q;
            *w++ = q;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static int ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* How many times `name` appears as a whole identifier. */
static int uses(const char *code, const char *name)
{
    const char *p = code;
    size_t      n = strlen(name);
    int         count = 0;

    while ((p = strstr(p, name)) != NULL) {
        if ((p == code || !ident_char(p[-1])) && !ident_char(p[n]))
            count++;
        p += n;
    }
    return count;
}

/*
 * What the command's own files may not name.  The openers first: a device
 * opened is the ZZ9000 defect, a library opened by path or name can load one
 * from disk and start its stack.  Then every tools.h helper that opens
 * bsdsocket.library the ordinary way, the probe, and the explainers that
 * reach the probe.
 */
static const char *const forbidden[] = {
    "OpenDevice", "OpenLibrary", "OldOpenLibrary", "OpenResource",
    "LoadSeg", "NewLoadSeg", "Execute", "SystemTagList", "System",
    "tool_device_probe", "ami_sana2_open_device",
    "ami_sana2_open_device_flags", "tool_open_library",
    "tool_netstatus_open", "tool_netstatus_system", "tool_stack_start",
    "tool_stack_hold", "tool_stack_add_interface", "tool_stack_query",
    "tool_stack_domain", "tool_stack_lookup", "tool_stack_lookup_addr",
    "tool_stack_name_servers", "tool_explain_device", "tool_scan_devices",
    "tool_explain_no_stack", "tool_require_stack", "ami_config_load",
    "ami_millis", "netstack_startup", "netstack_get",
    NULL
};

static const char *const command_files[] = {
    "src/tools/statusreport.c", "src/tools/statusreport_text.c",
    "src/tools/tool_passive.c", "src/tools/tool_sockets.c",
    "src/tools/statusreport_tasks.cpp", "src/tools/statusreport_frames.c",
    "third_party/shelltools/src/task_snapshot.cpp", NULL
};

static char *read_code(const char *rel)
{
    char  path[1024];
    char *src;

    snprintf(path, sizeof(path), "%s/%s", source_dir(), rel);
    src = slurp(path);
    CHECK(src != NULL, "cannot read %s", path);
    if (src != NULL)
        code_only(src);
    return src;
}

/* The body of `signature`, braces matched, or NULL. */
static char *function_body(const char *code, const char *signature)
{
    const char *p = strstr(code, signature);
    const char *open;
    int         depth = 0;
    const char *q;
    char       *body;

    if (p == NULL || (open = strchr(p, '{')) == NULL)
        return NULL;

    for (q = open; *q != '\0'; q++) {
        if (*q == '{')
            depth++;
        else if (*q == '}' && --depth == 0)
            break;
    }
    if (*q == '\0')
        return NULL;

    body = malloc((size_t)(q - open) + 2);
    if (body == NULL)
        return NULL;
    memcpy(body, open, (size_t)(q - open) + 1);
    body[q - open + 1] = '\0';
    return body;
}

static void check_passive(void)
{
    char *code;
    char *body;
    int   f;
    int   i;

    for (f = 0; command_files[f] != NULL; f++) {
        code = read_code(command_files[f]);
        if (code == NULL)
            continue;

        for (i = 0; forbidden[i] != NULL; i++)
            CHECK(uses(code, forbidden[i]) == 0,
                  "%s calls %s: this command reads, it does not open",
                  command_files[f], forbidden[i]);
        free(code);
    }

    /* The one opener it does use, and what that opener has to be. */
    code = read_code("src/tools/statusreport.c");
    if (code != NULL) {
        CHECK(uses(code, "tool_netstatus_open_resident") == 1,
              "statusreport.c reaches the stack through the resident opener, "
              "once");
        CHECK(uses(code, "tool_anxdiag_read") >= 1 &&
              uses(code, "tool_events_read") >= 1,
              "the driver records and the events come from tool_passive.c");
        free(code);
    }

    code = read_code("src/tools/tool_diag.c");
    if (code == NULL)
        return;

    body = function_body(code,
                         "struct Library *tool_netstatus_open_resident(VOID)");
    CHECK(body != NULL, "tool_diag.c has tool_netstatus_open_resident()");
    if (body != NULL) {
        const char *forbid  = strstr(body, "Forbid");
        const char *open    = strstr(body, "OpenLibrary");
        const char *permit  = strstr(body, "Permit");

        CHECK(uses(body, "OpenLibrary") == 1, "one OpenLibrary(), by name");
        CHECK(uses(body, "tool_open_library") == 0,
              "never the PROGDIR:-first opener");
        CHECK(uses(body, "FindName") == 1 && uses(body, "FindPort") >= 1 &&
              uses(body, "tool_stack_is_ours") == 1,
              "resident, running, and ours, before it opens");
        /* bsd_lib_open() blocks and releases only Exec's own Forbid(), so
           an open inside the caller's would wait with switching forbidden. */
        CHECK(forbid != NULL && open != NULL && permit != NULL &&
              forbid < permit && permit < open,
              "the lookup runs under Forbid(), the open after Permit()");
        CHECK(uses(body, "lib_IdString") >= 2 && uses(body, "CloseLibrary") == 1,
              "a base that is not the resident's is closed again");
        free(body);
    }
    free(code);
}

/* ------------------------------------------------------------- the tee --- */

typedef struct FakeHandle
{
    char buf[512];
    int  fail_after;            /* -1: never fails                          */
    int  calls;
} FakeHandle;

static LONG fake_put(APTR handle, const char *line)
{
    FakeHandle *h = (FakeHandle *)handle;

    if (h->fail_after >= 0 && h->calls++ >= h->fail_after)
        return -1;
    if (strlen(h->buf) + strlen(line) < sizeof(h->buf))
        strcat(h->buf, line);
    return 0;
}

static void check_tee(void)
{
    FakeHandle console = { "", -1, 0 };
    FakeHandle file    = { "", 1, 0 };     /* the second line fails */
    SrTee      t;
    SrOut      o;

    t.put = fake_put; t.console = &console; t.file = &file;
    t.file_failed = FALSE;
    o.write = sr_tee_write; o.user = &t; o.addresses = FALSE; o.lines = 0;

    sr_str(&o, "a", "1");
    CHECK(!t.file_failed, "a line that went out is not a failure");
    sr_str(&o, "b", "2");
    sr_str(&o, "c", "3");

    CHECK(t.file_failed, "a file line that did not go out is kept");
    CHECK(strcmp(console.buf, "a=1\nb=2\nc=3\n") == 0,
          "the console still gets every line, got '%s'", console.buf);
    CHECK(strcmp(file.buf, "a=1\n") == 0,
          "the file stops at the failure, got '%s'", file.buf);
    CHECK(file.calls == 2, "no write to the file after it failed (%d)",
          file.calls);

    console.buf[0] = '\0';
    t.file = NULL; t.file_failed = FALSE;
    sr_str(&o, "d", "4");
    CHECK(!t.file_failed && strcmp(console.buf, "d=4\n") == 0,
          "NOFILE: the console alone, and never a failure");
}

/* ------------------------------------------------------ tasks, sockets --- */

/* The ShowTasks DETAIL fields as task.N rows, and netstat ALL's table as
   socket.N rows with the peer address in the second class (#383). */
static void check_tasks_sockets(void)
{
    SrTask   t;
    SrSocket s;
    SrOut    o;

    memset(&t, 0, sizeof(t));
    t.address = 0x70675608UL; t.cli = 0; t.priority = 1; t.state = "WAIT";
    t.process = TRUE; t.sig_alloc = 0xfc00ffffUL; t.sig_wait = 0x04000000UL;
    t.stack_lower = 0x72c2f020UL; t.stack_upper = 0x72c6f020UL;
    t.stack_pointer = 0x72c6ef00UL; t.process_stack = 16384;
    t.name = "IBrowseNetwork"; t.command = "";
    o = sink(0);
    sr_task(&o, 7, &t);
    CHECK(has("task.7.name=IBrowseNetwork\n") && has("task.7.type=process\n") &&
          has("task.7.address=$70675608\n") && has("task.7.priority=1\n") &&
          has("task.7.state=WAIT\n") && has("task.7.sig_alloc=$fc00ffff\n") &&
          has("task.7.sig_wait=$04000000\n") && has("task.7.sig_recvd=$00000000\n") &&
          has("task.7.saved_sp=$72c6ef00\n") && has("task.7.process_stack=16384\n"),
          "a task as ShowTasks DETAIL has it: %s", out);
    CHECK(!has("task.7.command="), "no command line when there is none: %s", out);

    t.process = FALSE; t.command = "IBrowse";
    o = sink(0);
    sr_task(&o, 0, &t);
    CHECK(has("task.0.type=task\n") && has("task.0.command=IBrowse\n") &&
          !has("process_stack"), "a task has no process fields: %s", out);

    memset(&s, 0, sizeof(s));
    s.tcp = TRUE; s.state = "CLOSE_WAIT"; s.local_port = 49157;
    s.peer_port = 443; s.peer_address = 0x5e8a1d03UL; s.retransmits = 2;
    o = sink(0);
    sr_socket(&o, 1, &s);
    CHECK(has("socket.1.proto=tcp\n") && has("socket.1.state=CLOSE_WAIT\n") &&
          has("socket.1.local_port=49157\n") && has("socket.1.peer_port=443\n") &&
          has("socket.1.retransmits=2\n"), "a TCP row: %s", out);
    CHECK(!has("peer_address") && !has("94.138."),
          "no peer address without ADDRESSES: %s", out);

    o = sink(1);
    sr_socket(&o, 1, &s);
    CHECK(has("socket.1.peer_address=94.138.29.3\n"),
          "the peer address with ADDRESSES: %s", out);

    s.tcp = FALSE; s.local_port = 5353; s.queued = 3;
    o = sink(1);
    sr_socket(&o, 2, &s);
    CHECK(strcmp(out, "socket.2.proto=udp\nsocket.2.local_port=5353\n"
                      "socket.2.queued=3\n") == 0, "a UDP row: '%s'", out);
}

/* ------------------------------------------------------ return addresses --- */

/* A stretch of big-endian guest memory for sr_ranges_add() and the call
   decoding to read; anything outside it must not be read. */
#define FAKE_BASE   0x00400000UL
#define FAKE_SIZE   0x1000UL

static UBYTE fake_mem[FAKE_SIZE];
static int   fake_peeks;
static int   fake_refused;

static void put32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v;
}

static void poke(ULONG address, ULONG v)
{
    put32(fake_mem + (address - FAKE_BASE), v);
}

static BOOL fake_peek(APTR user, ULONG address, ULONG *value)
{
    const UBYTE *p;

    (void)user;
    fake_peeks++;
    if ((address & 1UL) != 0 || address < FAKE_BASE ||
        address - FAKE_BASE > FAKE_SIZE - 4UL) {
        fake_refused++;
        return FALSE;
    }
    p = fake_mem + (address - FAKE_BASE);
    *value = ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
             ((ULONG)p[2] << 8) | (ULONG)p[3];
    return TRUE;
}

/* One segment as LoadSeg() leaves it: the allocation's size, the next BPTR. */
static void fake_segment(ULONG seg, ULONG contents, ULONG next)
{
    poke(seg - 4UL, contents + 8UL);
    poke(seg, next >> 2);
}

#define LIB_SEG0    0x00400100UL    /* 0x100 bytes of contents */
#define LIB_SEG1    0x00400300UL    /* 0x40 */
#define APP_SEG0    0x00400500UL    /* 0x200 */

static SrRanges ranges;

static void fake_modules(void)
{
    memset(fake_mem, 0, sizeof(fake_mem));
    fake_segment(LIB_SEG0, 0x100UL, LIB_SEG1);
    fake_segment(LIB_SEG1, 0x40UL, 0);
    fake_segment(APP_SEG0, 0x200UL, 0);

    /* jsr abs.l ends at library hunk 0 + $20, jsr -126(a6) at the program's
       hunk 0 + $40, and bsr.s at + $50. */
    poke(LIB_SEG0 + 4UL + 0x20UL - 6UL, 0x4eb90040UL);
    poke(APP_SEG0 + 4UL + 0x40UL - 4UL, 0x4eaeff82UL);
    poke(APP_SEG0 + 4UL + 0x50UL - 4UL, 0x4e71610aUL);

    sr_ranges_init(&ranges, fake_peek, NULL);
    CHECK(sr_ranges_add(&ranges, "bsdsocket", TRUE, LIB_SEG0 >> 2) == 2,
          "the library's two hunks");
    CHECK(sr_ranges_add(&ranges, "Work:Web/I Brow=se", FALSE, APP_SEG0 >> 2) == 1,
          "the program's one hunk");
}

static int count_of(const char *text, char c)
{
    int n = 0;

    for (; *text != '\0'; text++)
        n += (*text == c);
    return n;
}

static void check_frames(void)
{
    static UBYTE stack[SR_STACK_BYTES];
    SrOut        o;
    ULONG        n;
    ULONG        i;

    fake_modules();

    o = sink(0);
    sr_modules(&o, &ranges);
    CHECK(strcmp(out, "module.count=2\n"
                      "module.0.name=bsdsocket\nmodule.0.hunk=0\n"
                      "module.0.start=$00400104\nmodule.0.size=256\n"
                      "module.1.name=bsdsocket\nmodule.1.hunk=1\n"
                      "module.1.start=$00400304\nmodule.1.size=64\n") == 0,
          "the library's hunks and nobody else's: '%s'", out);

    /* ---- one line, deepest first, at any even offset */
    memset(stack, 0, sizeof(stack));
    n = 0;
    put32(stack + n, 0x00000003UL);              n += 4;
    put32(stack + n, LIB_SEG0 + 4UL + 0x20UL);   n += 4;
    put32(stack + n, 0x12345678UL);              n += 4;
    put32(stack + n, LIB_SEG0 + 4UL + 0x21UL);   n += 4;   /* odd */
    put32(stack + n, LIB_SEG1 + 4UL + 0x10UL);   n += 4;
    put32(stack + n, LIB_SEG1 + 4UL + 0x40UL);   n += 4;   /* one past the end */
    n += 2;                                                /* a saved SR */
    put32(stack + n, APP_SEG0 + 4UL + 0x40UL);   n += 4;
    put32(stack + n, APP_SEG0 + 4UL + 0x50UL);   n += 4;

    o = sink(0);
    CHECK(sr_task_frames(&o, 12, &ranges, stack, n) &&
          strcmp(out, "task.12.frames=bsdsocket:0+$00000020 "
                      "bsdsocket:1+$00000010 I_Brow_se:0+$00000040/lvo-126 "
                      "I_Brow_se:0+$00000050\n") == 0 && o.lines == 1,
          "module:hunk+$offset, the vector of a jsr d(a6): '%s'", out);

    /* the last longword is cut short by the stack's upper bound */
    o = sink(0);
    CHECK(sr_task_frames(&o, 12, &ranges, stack, n - 2) &&
          !has("00000050"), "nothing is read past the copy: '%s'", out);

    /* ---- no frame in the library, no line */
    o = sink(0);
    CHECK(!sr_task_frames(&o, 3, &ranges, stack + 26, n - 26) &&
          out[0] == '\0' && o.lines == 0,
          "a task that is not in the library prints nothing: '%s'", out);
    o = sink(0);
    CHECK(!sr_task_frames(&o, 3, &ranges, stack, 0) && out[0] == '\0',
          "an empty stack prints nothing");

    /* ---- code is read only inside a range */
    fake_refused = 0;
    memset(stack, 0, sizeof(stack));
    put32(stack, LIB_SEG0 + 4UL);                /* offset 0: nothing before */
    put32(stack + 4, LIB_SEG0 + 4UL + 2UL);
    o = sink(0);
    fake_peeks = 0;
    CHECK(sr_task_frames(&o, 0, &ranges, stack, 8) && fake_peeks == 0 &&
          strcmp(out, "task.0.frames=bsdsocket:0+$00000000 "
                      "bsdsocket:0+$00000002\n") == 0,
          "no read before the start of a hunk: %d peeks, '%s'", fake_peeks, out);

    /* ---- SR_FRAMES_MAX at most */
    memset(stack, 0, sizeof(stack));
    for (i = 0; i < 20; i++)
        put32(stack + i * 4, LIB_SEG0 + 4UL + 0x80UL + i * 2UL);
    o = sink(0);
    CHECK(sr_task_frames(&o, 1, &ranges, stack, 80) &&
          count_of(out, ' ') == SR_FRAMES_MAX - 1 &&
          has("=bsdsocket:0+$00000080 ") && has(" bsdsocket:0+$0000009e\n") &&
          !has("000000a0"), "the first sixteen of twenty: '%s'", out);

    /* a frame a call comes before is not crowded out by ones none does */
    put32(stack + 80, APP_SEG0 + 4UL + 0x40UL);
    o = sink(0);
    CHECK(sr_task_frames(&o, 1, &ranges, stack, 84) &&
          count_of(out, ' ') == SR_FRAMES_MAX - 1 &&
          has(" bsdsocket:0+$0000009c I_Brow_se:0+$00000040/lvo-126\n"),
          "the call site survives the cut: '%s'", out);

    /* a whole stack of candidates neither overruns nor prints a long line */
    for (i = 0; i < SR_STACK_BYTES / 4; i++)
        put32(stack + i * 4, LIB_SEG0 + 4UL + 0x20UL);
    o = sink(0);
    CHECK(sr_task_frames(&o, 95, &ranges, stack, SR_STACK_BYTES) &&
          count_of(out, ' ') == SR_FRAMES_MAX - 1 && count_of(out, '\n') == 1,
          "sixteen of 256: '%s'", out);
}

static void check_seglists(void)
{
    static SrRanges t;

    fake_modules();

    /* the same seglist again, as every process of one program has it */
    CHECK(sr_ranges_add(&ranges, "again", FALSE, APP_SEG0 >> 2) == 0 &&
          ranges.count == 3, "a seglist already in the table adds nothing");

    sr_ranges_init(&t, fake_peek, NULL);
    CHECK(sr_ranges_add(&t, "x", FALSE, 0) == 0, "no seglist");
    CHECK(sr_ranges_add(&t, "x", FALSE, 0x40000001UL) == 0 &&
          sr_ranges_add(&t, "x", FALSE, 0xffffffffUL) == 0,
          "a BPTR with a top bit set is not followed");
    CHECK(sr_ranges_add(&t, "x", FALSE, 1) == 0, "nor one below any segment");
    CHECK(sr_ranges_add(&t, "x", FALSE, 0x00500000UL >> 2) == 0,
          "nor one into memory that must not be read");

    /* a size no allocation has */
    poke(APP_SEG0 - 4UL, 4UL);
    CHECK(sr_ranges_add(&t, "x", FALSE, APP_SEG0 >> 2) == 0, "size 4");
    poke(APP_SEG0 - 4UL, SR_HUNK_BYTES_MAX + 4UL);
    CHECK(sr_ranges_add(&t, "x", FALSE, APP_SEG0 >> 2) == 0, "size 16 MB + 4");
    poke(APP_SEG0 - 4UL, 0xfffffff0UL);
    CHECK(sr_ranges_add(&t, "x", FALSE, APP_SEG0 >> 2) == 0 && t.count == 0,
          "size -16");

    /* the first hunk is good and its link is not: the walk ends there */
    fake_segment(APP_SEG0, 0x200UL, 0);
    poke(LIB_SEG0, 0x00123456UL);
    CHECK(sr_ranges_add(&t, "x", FALSE, LIB_SEG0 >> 2) == 1 && t.count == 1 &&
          t.r[0].start == LIB_SEG0 + 4UL && t.r[0].size == 0x100UL,
          "a corrupt link ends the walk after the hunks before it");

    /* a seglist that links to itself */
    sr_ranges_init(&t, fake_peek, NULL);
    poke(APP_SEG0, APP_SEG0 >> 2);
    CHECK(sr_ranges_add(&t, "x", FALSE, APP_SEG0 >> 2) == SR_HUNKS_MAX &&
          t.r[SR_HUNKS_MAX - 1].hunk == SR_HUNKS_MAX - 1,
          "a loop stops at the hunk cap");

    /* and the table itself is a cap */
    sr_ranges_init(&t, fake_peek, NULL);
    poke(LIB_SEG1, LIB_SEG1 >> 2);
    t.count = SR_RANGES_MAX - 3;
    CHECK(sr_ranges_add(&t, "y", FALSE, LIB_SEG1 >> 2) == 3 &&
          t.count == SR_RANGES_MAX, "a full table takes no more");
    CHECK(sr_ranges_add(&t, "z", FALSE, LIB_SEG0 >> 2) == 0 &&
          t.count == SR_RANGES_MAX, "nor does a later seglist");

    /* an empty hunk keeps its index */
    sr_ranges_init(&t, fake_peek, NULL);
    fake_segment(LIB_SEG0, 0, LIB_SEG1);
    fake_segment(LIB_SEG1, 0x40UL, 0);
    CHECK(sr_ranges_add(&t, "x", FALSE, LIB_SEG0 >> 2) == 1 && t.r[0].hunk == 1,
          "an empty hunk is counted and not listed");
}

static void check_openers(void)
{
    SrOpener op;
    SrTask   t;
    SrOut    o;

    op.task = 0x70675608UL; op.name = "IBrowseNetwork"; op.sockets = 2;
    op.break_mask = 0x00001000UL; op.flags = 0x0002UL;
    o = sink(0);
    sr_opener(&o, 3, &op);
    CHECK(strcmp(out, "opener.3.task=$70675608\nopener.3.name=IBrowseNetwork\n"
                      "opener.3.sockets=2\nopener.3.break_mask=$00001000\n"
                      "opener.3.flags=$00000002\n") == 0, "an opener: '%s'", out);

    /* opener.N.task is task.N.address, digit for digit */
    memset(&t, 0, sizeof(t));
    t.address = op.task; t.state = "WAIT"; t.name = "IBrowseNetwork"; t.command = "";
    sr_task(&o, 7, &t);
    CHECK(has("task.7.address=$70675608\n"), "the two join: %s", out);
}

/* ------------------------------------------------------------------ main --- */

/* The DHCP REQUEST checkpoints and the resident library's build (#306). */
static void check_checkpoints(void)
{
    static const UBYTE opt54[8] = { 0x36, 0x04, 0xc0, 0xa8, 0x00, 0x01, 0x37, 0x05 };
    static const char  id[] =
        "bsdsocket.library 1.0.0 (AmiNetXDuo, ABI 4.12, build 6275a6a8, profile micro)\r\n";
    SrOut o;
    char  field[32];
    char *src;
    char  path[1024];

    o = sink(0);
    sr_addr_bytes(&o, "dhcp.eth0.req_opt54_built", opt54, sizeof(opt54));
    CHECK(out[0] == '\0', "option bytes hold an address: none without ADDRESSES: '%s'", out);

    o = sink(1);
    sr_addr_bytes(&o, "dhcp.eth0.req_opt54_built", opt54, sizeof(opt54));
    sr_addr_bytes(&o, "dhcp.eth0.req_opts_final", NULL, 24);
    CHECK(strcmp(out, "dhcp.eth0.req_opt54_built=3604c0a800013705\n"
                      "dhcp.eth0.req_opts_final=unavailable\n") == 0,
          "option bytes as hex: '%s'", out);

    CHECK(sr_id_field(id, "build", field, sizeof(field)) && strcmp(field, "6275a6a8") == 0,
          "build from the IdString: '%s'", field);
    CHECK(sr_id_field(id, "profile", field, sizeof(field)) && strcmp(field, "micro") == 0,
          "profile from the IdString: '%s'", field);
    CHECK(!sr_id_field("bsdsocket.library 0.29 (AmiNetXDuo, ABI 4.11)\r\n", "build",
                       field, sizeof(field)), "an older IdString has no build");
    CHECK(!sr_id_field("bsdsocket.library 1.0 (AmiNetXDuo, ABI 4.12, build , profile fat)",
                       "build", field, sizeof(field)), "an empty build is no build");

    /* The report prints the four keys and the library carries the two fields. */
    snprintf(path, sizeof(path), "%s/src/tools/statusreport.c", source_dir());
    src = slurp(path);
    CHECK(src != NULL, "read %s", path);
    if (src != NULL)
    {
        CHECK(strstr(src, "\"req_opt54_index_before\"") && strstr(src, "\"req_opt54_index_after\"") &&
              strstr(src, "\"req_opt54_built\"") && strstr(src, "\"req_opts_final\""),
              "statusreport.c prints the four req_ keys");
        free(src);
    }
    snprintf(path, sizeof(path), "%s/src/bsdsocket/library.c", source_dir());
    src = slurp(path);
    CHECK(src != NULL && strstr(src, "\", build \" AMINETXDUO_VERSION_HASH") &&
          strstr(src, "\", profile \" AMINETXDUO_VERSION_PROFILE"),
          "the IdString carries build and profile");
    free(src);
}

int main(int argc, char **argv)
{
    const char *group = (argc > 1) ? argv[1] : NULL;

    if (group == NULL || strcmp(group, "allowlist") == 0) {
        check_allowlist();
        check_lines();
        check_decoding();
        check_tee();
        check_checkpoints();
        check_tasks_sockets();
        check_frames();
        check_seglists();
        check_openers();
    }
    if (group == NULL || strcmp(group, "passive") == 0)
        check_passive();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
