/*
 * AmiNetXDuo, bpf ABI pin.
 *
 * Amiga-only, and a translation unit of its own so that every assertion runs
 * against the Roadshow NDK <net/bpf.h> itself. A consumer walks the buffer
 * with BPF_WORDALIGN(bh_hdrlen + bh_caplen) and reads data at
 * record + bh_hdrlen, so bh_hdrlen must be 20, not 18 or 24.
 *
 * Measured 2026-07-24 with m68k-amigaos-gcc 15.2.0 -c -O2 -m68020.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/bpf.h"

#include <stddef.h>

#ifdef AMI_BPF_REPLICA
#  error "bpf_abi_check.c is only meaningful against the real NDK <net/bpf.h>"
#endif

/* struct bpf_hdr, 8 + 4 + 4 + 2 = 18 bytes on 68k, where a struct needs no
   more than 2-byte alignment. BPF_WORDALIGN rounds it to 20 for the record. */
AMI_STATIC_ASSERT(sizeof(struct bpf_hdr) == AMI_BPF_HDR_BYTES, "bpf_hdr size");
AMI_STATIC_ASSERT(offsetof(struct bpf_hdr, bh_tstamp)  == AMI_BPF_OFF_TSTAMP_SEC, "bh_tstamp");
AMI_STATIC_ASSERT(offsetof(struct bpf_hdr, bh_caplen)  == AMI_BPF_OFF_CAPLEN,  "bh_caplen");
AMI_STATIC_ASSERT(offsetof(struct bpf_hdr, bh_datalen) == AMI_BPF_OFF_DATALEN, "bh_datalen");
AMI_STATIC_ASSERT(offsetof(struct bpf_hdr, bh_hdrlen)  == AMI_BPF_OFF_HDRLEN,  "bh_hdrlen");
AMI_STATIC_ASSERT(sizeof(((struct bpf_hdr *)0)->bh_caplen)  == 4, "bh_caplen width");
AMI_STATIC_ASSERT(sizeof(((struct bpf_hdr *)0)->bh_datalen) == 4, "bh_datalen width");
AMI_STATIC_ASSERT(sizeof(((struct bpf_hdr *)0)->bh_hdrlen)  == 2, "bh_hdrlen width");

/* bh_tstamp is a `struct timeval`, the Amiga one, two ULONGs. If a toolchain
   substitutes a Unix timeval with a 64-bit time_t, the record grows silently
   and every consumer misparses it. */
AMI_STATIC_ASSERT(sizeof(((struct bpf_hdr *)0)->bh_tstamp) == 8, "bh_tstamp size");

/* struct bpf_insn, an array of these is copied verbatim out of application
   memory by BIOCSETF, so the width of every field matters. Note that `code`
   is a 16-bit UWORD here, not the 32-bit int that some BPF headers use. */
AMI_STATIC_ASSERT(sizeof(struct bpf_insn) == 8, "bpf_insn size");
AMI_STATIC_ASSERT(offsetof(struct bpf_insn, code) == 0, "insn.code");
AMI_STATIC_ASSERT(offsetof(struct bpf_insn, jt)   == 2, "insn.jt");
AMI_STATIC_ASSERT(offsetof(struct bpf_insn, jf)   == 3, "insn.jf");
AMI_STATIC_ASSERT(offsetof(struct bpf_insn, k)    == 4, "insn.k");
AMI_STATIC_ASSERT(sizeof(((struct bpf_insn *)0)->code) == 2, "insn.code width");
AMI_STATIC_ASSERT(sizeof(((struct bpf_insn *)0)->jt)   == 1, "insn.jt width");
AMI_STATIC_ASSERT(sizeof(((struct bpf_insn *)0)->k)    == 4, "insn.k width");

/* struct bpf_program, as passed to BIOCSETF. */
AMI_STATIC_ASSERT(sizeof(struct bpf_program) == 8, "bpf_program size");
AMI_STATIC_ASSERT(offsetof(struct bpf_program, bf_len)   == 0, "bf_len");
AMI_STATIC_ASSERT(offsetof(struct bpf_program, bf_insns) == 4, "bf_insns");

/* struct bpf_stat, as returned by BIOCGSTATS. */
AMI_STATIC_ASSERT(sizeof(struct bpf_stat) == 8, "bpf_stat size");
AMI_STATIC_ASSERT(offsetof(struct bpf_stat, bs_recv) == 0, "bs_recv");
AMI_STATIC_ASSERT(offsetof(struct bpf_stat, bs_drop) == 4, "bs_drop");

/* Alignment and the derived record header length. */
AMI_STATIC_ASSERT(BPF_ALIGNMENT == 4, "BPF_ALIGNMENT");
AMI_STATIC_ASSERT(BPF_WORDALIGN(18) == 20, "BPF_WORDALIGN(18)");
AMI_STATIC_ASSERT(BPF_WORDALIGN(20) == 20, "BPF_WORDALIGN(20)");
AMI_STATIC_ASSERT(BPF_WORDALIGN(21) == 24, "BPF_WORDALIGN(21)");
AMI_STATIC_ASSERT(AMI_BPF_HDRLEN == 20, "bh_hdrlen");

/* Limits and instruction encodings the validator and the VM key on. */
AMI_STATIC_ASSERT(BPF_MAXINSNS   == 512,    "BPF_MAXINSNS");
AMI_STATIC_ASSERT(BPF_MAXBUFSIZE == 0x8000, "BPF_MAXBUFSIZE");
AMI_STATIC_ASSERT(BPF_MINBUFSIZE == 32,     "BPF_MINBUFSIZE");
AMI_STATIC_ASSERT(BPF_MEMWORDS   == 16,     "BPF_MEMWORDS");
AMI_STATIC_ASSERT(BPF_LD == 0x00 && BPF_LDX == 0x01 && BPF_ST == 0x02 &&
                  BPF_STX == 0x03 && BPF_ALU == 0x04 && BPF_JMP == 0x05 &&
                  BPF_RET == 0x06 && BPF_MISC == 0x07, "instruction classes");
AMI_STATIC_ASSERT(BPF_W == 0x00 && BPF_H == 0x08 && BPF_B == 0x10, "sizes");
AMI_STATIC_ASSERT(BPF_IMM == 0x00 && BPF_ABS == 0x20 && BPF_IND == 0x40 &&
                  BPF_MEM == 0x60 && BPF_LEN == 0x80 && BPF_MSH == 0xa0, "modes");
AMI_STATIC_ASSERT(BPF_K == 0x00 && BPF_X == 0x08 && BPF_A == 0x10, "src/rval");
AMI_STATIC_ASSERT(DLT_EN10MB == 1 && DLT_NULL == 0, "link types");

/* The ioctl encodings. A caller built against a different struct ifreq
   disagrees on IOCPARM_LEN, so ami_bpf_ioctl() dispatches on direction, group
   and number, and ignores the length. */
AMI_STATIC_ASSERT(IOCGROUP(BIOCGBLEN)    == 'B', "BIOCGBLEN group");
AMI_STATIC_ASSERT(IOCPARM_LEN(BIOCSETIF) == 32,  "sizeof(struct ifreq)");
AMI_STATIC_ASSERT(IOCPARM_LEN(BIOCGSTATS) == 8,  "sizeof(struct bpf_stat)");
AMI_STATIC_ASSERT(IOCPARM_LEN(BIOCSETF)   == 8,  "sizeof(struct bpf_program)");
AMI_STATIC_ASSERT((BIOCGBLEN & IOC_DIRMASK) == IOC_OUT,   "BIOCGBLEN direction");
AMI_STATIC_ASSERT((BIOCSBLEN & IOC_DIRMASK) == IOC_INOUT, "BIOCSBLEN direction");
AMI_STATIC_ASSERT(BIOCGBLEN != BIOCSBLEN, "BIOCGBLEN/BIOCSBLEN must be distinct");
AMI_STATIC_ASSERT(IFNAMSIZ == 16, "IFNAMSIZ");

const char ami_bpf_abi_checked[] = "bpf ABI pinned against ndk-include/net/bpf.h";
