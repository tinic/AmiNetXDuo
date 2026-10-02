/*
 * AmiNetXDuo, ICMPv6 ND option-tail validation: the validator must reject a
 * 1-2 octet option tail so the NA/NS/RA/redirect walkers never re-walk a
 * fragment.  Each walker loops on its own `> 0` bound and strides by the
 * option's length byte; a zero length byte never advances the pointer, so a
 * 2-octet tail `{type=3, len=0}` spins the receive thread under
 * nx_ip_protection, and a tail whose length byte claims a whole option makes
 * the redirect walker wrap its ULONG count.  This drives the real
 * _nx_icmpv6_validate_options directly: it must reject both, while still
 * accepting every whole 8-octet-aligned option.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ipv6.h"
#include "nx_icmpv6.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}

/* Drive the validator on a raw option area so the crafted tails are exact
   byte sequences, not the struct's 8-octet view of a whole option. */
static UINT h_validate(UCHAR *buf, INT length)
{
    return _nx_icmpv6_validate_options((NX_ICMPV6_OPTION *)buf, length, 0);
}

int main(void)
{
    NX_ICMPV6_OPTION *opt;
    UCHAR buf[32];
    UINT rc;

    printf("ICMPv6 ND option-tail validation, direct validator contract\n");

    /* A single whole 8-octet prefix option is valid. */
    memset(buf, 0, sizeof(buf));
    opt = (NX_ICMPV6_OPTION *)buf;
    opt->nx_icmpv6_option_type   = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    opt->nx_icmpv6_option_length = 1;
    rc = h_validate(buf, 8);
    h_check(rc == NX_SUCCESS, "a whole prefix option must validate");

    /* Two whole options. */
    memset(buf, 0, sizeof(buf));
    opt = (NX_ICMPV6_OPTION *)buf;
    opt->nx_icmpv6_option_type   = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    opt->nx_icmpv6_option_length = 1;
    opt = (NX_ICMPV6_OPTION *)(buf + 8);
    opt->nx_icmpv6_option_type   = ICMPV6_OPTION_TYPE_SRC_LINK_ADDR;
    opt->nx_icmpv6_option_length = 1;
    rc = h_validate(buf, 16);
    h_check(rc == NX_SUCCESS, "two whole options must validate");

    /* An empty option area (no options) is valid. */
    rc = h_validate(buf, 0);
    h_check(rc == NX_SUCCESS, "an empty option area must validate");

    /* The NA repro tail: type=prefix, length byte zero.  The old validator
       returned SUCCESS, and the NA walker then strides by 0 and never ends. */
    buf[0] = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    buf[1] = 0;
    rc = h_validate(buf, 2);
    h_check(rc == NX_NOT_SUCCESSFUL, "a 2-octet tail (zero length byte) must be rejected");

    /* A 1-octet tail. */
    buf[0] = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    rc = h_validate(buf, 1);
    h_check(rc == NX_NOT_SUCCESSFUL, "a 1-octet tail must be rejected");

    /* A 2-octet tail whose length byte claims a whole option: the redirect
       walker strides 8 into a 2-octet field and wraps its ULONG count. */
    buf[0] = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    buf[1] = 1;
    rc = h_validate(buf, 2);
    h_check(rc == NX_NOT_SUCCESSFUL,
            "a 2-octet tail claiming a whole option must be rejected");

    /* An option whose length byte claims more than remains (overrun).  Both
       old and new reject; a regression guard. */
    memset(buf, 0, sizeof(buf));
    opt = (NX_ICMPV6_OPTION *)buf;
    opt->nx_icmpv6_option_type   = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    opt->nx_icmpv6_option_length = 2;   /* claims 16 octets, 8 present */
    rc = h_validate(buf, 8);
    h_check(rc == NX_NOT_SUCCESSFUL, "an overrun length byte must be rejected");

    /* A zero length byte inside a whole option (the GHSA-rf32-h832-hg8r
       guard).  Both old and new reject; a regression guard. */
    memset(buf, 0, sizeof(buf));
    opt = (NX_ICMPV6_OPTION *)buf;
    opt->nx_icmpv6_option_type   = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    opt->nx_icmpv6_option_length = 0;
    rc = h_validate(buf, 8);
    h_check(rc == NX_NOT_SUCCESSFUL, "a zero length byte must be rejected");

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
