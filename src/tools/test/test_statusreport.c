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
    "src/tools/tool_passive.c", NULL
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

/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    const char *group = (argc > 1) ? argv[1] : NULL;

    if (group == NULL || strcmp(group, "allowlist") == 0) {
        check_allowlist();
        check_lines();
        check_decoding();
        check_tee();
    }
    if (group == NULL || strcmp(group, "passive") == 0)
        check_passive();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
