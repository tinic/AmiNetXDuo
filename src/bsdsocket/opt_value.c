/*
 * bsdsocket.library, integer socket-option replies.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

LONG bsd_opt_get_long(struct AmiSocketBase *base, APTR optval,
                             socklen_t *optlen, LONG value)
{
    socklen_t len;

    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    len = *optlen;
    if (len >= (socklen_t)sizeof(LONG))
    {
        bsd_bcopy(&value, optval, sizeof(value));
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (len >= (socklen_t)sizeof(WORD))
    {
        WORD short_value = (WORD)value;

        bsd_bcopy(&short_value, optval, sizeof(short_value));
        *optlen = (socklen_t)sizeof(WORD);
    }
    else
    {
        return bsd_fail(base, AMI_EINVAL);
    }

    return 0;
}

