/*
 * AmiNetXDuo -- the ThreadX priority ladder, in one place.
 *
 * This header exists because the ladder used to live in two: netstack_internal.h
 * defined the IP thread's priority and sana2_internal.h the readers', each with
 * a comment describing their relationship, and the two drifted into stating an
 * ordering the numbers did not implement. It cost 6.6% of bulk receive
 * throughput and went unnoticed for months precisely because every reading of
 * either comment confirmed the intent (docs/RESEARCH.md 52).
 *
 * So the ordering is declared once, and asserted below rather than described.
 * Both internal headers include this and neither defines a priority.
 *
 * ThreadX counts DOWN: lowest number wins. From the scheduler itself,
 * tx_thread_system_resume.c:213 --
 *
 *     if (priority < _tx_thread_highest_priority)
 *         // A new highest priority thread is present.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_THREAD_PRIORITIES_H
#define AMINETXDUO_THREAD_PRIORITIES_H

/*
 * The SANA-II readers sit at the top. The device has no buffers of its own and
 * drops any frame arriving with no CMD_READ outstanding, so a reader that
 * cannot get the CPU during a burst loses packets on the wire and the far end
 * retransmits. Nothing may preempt them -- least of all the IP thread, which
 * has continuous work during a bulk transfer and would otherwise starve the
 * threads feeding it.
 */
#define AMI_SANA2_RX_PRIORITY       1

/* The IP thread outranks everything that CONSUMES packets, and nothing else. */
#define AMI_IP_THREAD_PRIORITY      2

#define AMI_AUTOIP_PRIORITY         3
#define AMI_CALLER_PRIORITY        16      /* adopted application tasks      */

/*
 * The invariants, so that the next edit to the numbers above fails to compile
 * instead of costing another 6.6%.
 */
#if AMI_SANA2_RX_PRIORITY >= AMI_IP_THREAD_PRIORITY
#error "SANA-II readers must outrank the IP thread (ThreadX: lower number wins)"
#endif
#if AMI_IP_THREAD_PRIORITY >= AMI_CALLER_PRIORITY
#error "the IP thread must outrank adopted callers"
#endif
#if AMI_IP_THREAD_PRIORITY >= AMI_AUTOIP_PRIORITY
#error "the IP thread must outrank AutoIP"
#endif

#endif /* AMINETXDUO_THREAD_PRIORITIES_H */
