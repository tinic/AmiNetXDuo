#ifndef HEADER_CURL_AMITLS_H
#define HEADER_CURL_AMITLS_H
/*
 * curl's vtls backend over AmiNetXDuo's tls.library.
 *
 * This file is NOT part of curl.  It is copied into
 * third_party/curl/lib/vtls/ by clients/curl/build.sh -t, together with
 * clients/curl/curl-amitls.patch, which is what teaches curl's build system
 * and backend table that it exists.  See clients/curl/amitls.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "curl_setup.h"

#ifdef USE_AMITLS

extern const struct Curl_ssl Curl_ssl_amitls;

#endif /* USE_AMITLS */
#endif /* HEADER_CURL_AMITLS_H */
