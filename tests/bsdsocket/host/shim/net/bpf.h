/* <net/bpf.h> for the bsdsocket host tests.  The filter program shape only;
   the VM itself is tested by tests/bpf against src/bpf.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_NET_BPF_H
#define AMINETXDUO_BSD_TEST_NET_BPF_H
#include <exec/types.h>
struct bpf_insn { UWORD code; UBYTE jt; UBYTE jf; ULONG k; };
struct bpf_program { ULONG bf_len; struct bpf_insn *bf_insns; };
#endif
