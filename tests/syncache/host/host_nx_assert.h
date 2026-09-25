/*
 * Force-included into test_syncache_detach only: NX_ASSERT_FAIL reports and
 * unwinds to the test instead of sleeping forever, which is what nx_api.h
 * makes it on the target.  The product's definition is untouched.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HOST_NX_ASSERT_H
#define HOST_NX_ASSERT_H

void host_nx_assert_fail(const char *file, int line) __attribute__((noreturn));

#define NX_ASSERT_FAIL  { host_nx_assert_fail(__FILE__, __LINE__); }

#endif
