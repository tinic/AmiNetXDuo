/* <inline/alib.h> for the sana2 host tests.

   On the target this header is the NDK's, and BeginIO() there is a macro over
   the device's own vector.  Here it is a declaration the test binary defines,
   the same way this shim's <proto/exec.h> handles SendIO(): the point of
   compiling src/sana2 with BeginIO() rather than SendIO() is that io_Flags
   survives the post, and a test that has to see the flag has to see the call.

   SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_SANA2_TEST_INLINE_ALIB_H
#define AMINETXDUO_SANA2_TEST_INLINE_ALIB_H

#include <exec/types.h>
#include <exec/io.h>

VOID BeginIO(struct IORequest *req);

#endif
