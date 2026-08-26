/*
 * A counter that reaches no command reaches nobody: every diagnostic is
 * compiled out of a shipped build, so netstat and ShowNetStatus are the only
 * channel a user in the field can quote back.
 *
 * The selectors themselves are asserted in tests/bsdsocket/host. This asserts
 * the other half -- that the commands ASK and PRINT -- by reading their
 * source, for test_refusals.c's reason: netstat.c and shownetstatus.c are
 * ReadArgs, dos.library and Exec, and compile nowhere but the target.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AMINETXDUO_SOURCE_DIR
#define AMINETXDUO_SOURCE_DIR "."
#endif

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

static const char *source_dir(void)
{
    const char *env = getenv("AMINETXDUO_SOURCE_DIR");

    return (env != NULL && env[0] != '\0') ? env : AMINETXDUO_SOURCE_DIR;
}

/* Comments first: a phrase asserted below has to be in code the command runs,
   not in a sentence about it. Literals are kept, they are what is printed. */
static void strip_comments(char *s)
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
        if (*r == '"') {
            *w++ = *r++;
            while (*r != '\0' && *r != '"') {
                if (*r == '\\' && r[1] != '\0')
                    *w++ = *r++;
                *w++ = *r++;
            }
            if (*r == '\0')
                break;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static char *slurp(const char *rel)
{
    char   path[1024];
    FILE  *f;
    long   n;
    char  *buf;

    snprintf(path, sizeof(path), "%s/%s", source_dir(), rel);

    f = fopen(path, "rb");
    if (f == NULL) {
        printf("  FAIL cannot read %s\n", path);
        failures++;
        checks++;
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = malloc((size_t)n + 1);
    if (buf == NULL || n < 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("  FAIL cannot read %s\n", path);
        failures++;
        checks++;
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);

    strip_comments(buf);
    return buf;
}

static void wants(const char *rel, const char *what, const char *why)
{
    char *src = slurp(rel);

    if (src == NULL)
        return;

    CHECK(strstr(src, what) != NULL, "%s does not %s (%s)", rel, why, what);
    free(src);
}

int main(void)
{
    /* NETSTATUS_DHCP6 landed with nothing reading it: both commands showed
       the IPv4 lease only. */
    wants("src/tools/tool_nx.c", "NETSTATUS_DHCP6",
          "ask for the DHCPv6 lease");
    wants("src/tools/tool_nx.c", "tool_print_lease6",
          "carry the one lease printer");
    wants("src/tools/shownetstatus.c", "tool_dhcp6",
          "read the DHCPv6 lease table");
    wants("src/tools/shownetstatus.c", "tool_print_lease6",
          "print the DHCPv6 lease");
    wants("src/tools/shownetstatus.c", "lease6",
          "label the DHCPv6 lease");
    wants("src/tools/netstat.c", "tool_dhcp6",
          "read the DHCPv6 lease table");
    wants("src/tools/netstat.c", "tool_print_lease6",
          "print the DHCPv6 lease");
    wants("src/tools/netstat.c", "lease6",
          "label the DHCPv6 lease");

    /* The tick task's catch-up count reached only a serial dump that a
       shipping build compiles out. netstat -h is the wire it crosses now. */
    wants("src/tools/tool_nx.c", "nsl_TickCatchups",
          "carry the tick catch-ups out of NETSTATUS_HEALTH");
    wants("src/tools/tool_nx.c", "tx_amiga_tick_catchups",
          "read them off the published mark as well");
    wants("src/tools/netstat.c", "tick_catchups",
          "print the tick catch-up count");
    wants("src/tools/netstat.c", "catch-ups",
          "label the tick catch-up count");

    /* The TX leg of a received segment. A leg that stops at ami_budget is a
       leg no operator can quote, so the whole chain is asserted: armed,
       consumed, copied into NETSTATUS_RXBUDGET, printed by netstat -s. */
    wants("src/common/budget.c", "ami_budget_xmit",
          "close the transmit leg");
    wants("src/common/budget.c", "xmit_at",
          "arm the transmit leg at socket entry");
    wants("src/sana2/sana2_tx.c", "ami_budget_xmit",
          "consume the stamp where the driver call starts");
    wants("src/bsdsocket/netstatus.c", "nrb_Xmit",
          "carry the transmit leg out of NETSTATUS_RXBUDGET");
    wants("src/tools/netstat.c", "nrb_Xmit",
          "read the transmit leg");
    wants("src/tools/netstat.c", "xmit,",
          "label the transmit leg");

    printf("%s: %d checks, %d failures\n", "leasesurface", checks, failures);
    return (failures == 0) ? 0 : 1;
}
