/*
 * ami_syslog_level(), the BSD syslog priority -> log level mapping.
 *
 * WHY THIS EXISTS AT ALL
 *
 *   vsyslog was bsd_enosys for the whole life of the vector table, and what
 *   found it was a survey of Aminet daemons rather than any test here --
 *   telnetd calls it 4 times, lpd 8, AmiFTPd 28 or more, and every line went
 *   nowhere.  A gap only an external corpus could find is worth pinning down
 *   once it is closed.
 *
 * WHY THE MAPPING AND NOT THE OUTPUT
 *
 *   Where the text goes is RawDoFmt's business and the serial port's, and
 *   asserting it would only restate the call.  What can be wrong is the
 *   arithmetic: syslog.h packs a facility and a severity into one value and
 *   the severity is the low three bits.  A daemon logging LOG_LOCAL0|LOG_ERR
 *   sends 131, and 131 read whole is not a severity at all.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/syslog_level.h"

#include <stdio.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);          \
        }                                                                     \
    } while (0)

int main(void)
{
    unsigned long f;

    /* Severity alone, LOG_EMERG(0) .. LOG_DEBUG(7). */
    CHECK(ami_syslog_level(0) == AMI_SYSLOG_LEVEL_ERROR, "LOG_EMERG is error");
    CHECK(ami_syslog_level(1) == AMI_SYSLOG_LEVEL_ERROR, "LOG_ALERT is error");
    CHECK(ami_syslog_level(2) == AMI_SYSLOG_LEVEL_ERROR, "LOG_CRIT is error");
    CHECK(ami_syslog_level(3) == AMI_SYSLOG_LEVEL_ERROR, "LOG_ERR is error");
    CHECK(ami_syslog_level(4) == AMI_SYSLOG_LEVEL_WARN,  "LOG_WARNING warns");
    CHECK(ami_syslog_level(5) == AMI_SYSLOG_LEVEL_INFO,  "LOG_NOTICE informs");
    CHECK(ami_syslog_level(6) == AMI_SYSLOG_LEVEL_INFO,  "LOG_INFO informs");
    CHECK(ami_syslog_level(7) == AMI_SYSLOG_LEVEL_INFO,  "LOG_DEBUG informs");

    /*
     * THE POINT OF THE FILE.  A daemon logs facility|severity, so the facility
     * bits must not reach the decision.  LOG_LOCAL0 is 16<<3 = 128, so
     * LOG_LOCAL0|LOG_ERR is 131; a mapping that read the value whole would
     * fall off its own switch and call it informational.
     */
    for (f = 0; f <= 23; f++) {
        unsigned long fac = f << 3;

        CHECK(ami_syslog_level(fac | 0UL) == AMI_SYSLOG_LEVEL_ERROR,
              "facility|LOG_EMERG");
        CHECK(ami_syslog_level(fac | 3UL) == AMI_SYSLOG_LEVEL_ERROR,
              "facility|LOG_ERR");
        CHECK(ami_syslog_level(fac | 4UL) == AMI_SYSLOG_LEVEL_WARN,
              "facility|LOG_WARNING");
        CHECK(ami_syslog_level(fac | 6UL) == AMI_SYSLOG_LEVEL_INFO,
              "facility|LOG_INFO");
        CHECK(ami_syslog_level(fac | 7UL) == AMI_SYSLOG_LEVEL_INFO,
              "facility|LOG_DEBUG");
    }

    /* LOG_LOCAL0|LOG_ERR by name, because it is the case in the comment. */
    CHECK(ami_syslog_level(131UL) == AMI_SYSLOG_LEVEL_ERROR,
          "LOG_LOCAL0|LOG_ERR is 131 and is an error");

    printf("%s: %lu checks, %lu failures\n",
           h_failures ? "FAIL" : "ok", h_checks, h_failures);
    return h_failures ? 1 : 0;
}
