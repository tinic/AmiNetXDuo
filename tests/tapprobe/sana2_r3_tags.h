/*
 * The SANA-II R3 buffer-management tags src/sana2/sana2_device.h stops short
 * of.  Our stack offers none of them, so it has no reason to carry them;
 * Roadshow offers two, which is why the probe has to name them.
 *
 * Include after sana2_device.h, which defines S2_Dummy.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TAPPROBE_SANA2_R3_TAGS_H
#define TAPPROBE_SANA2_R3_TAGS_H

#ifndef S2_CopyToBuff32
#define S2_CopyToBuff32         (S2_Dummy + 6)
#endif
#ifndef S2_CopyFromBuff32
#define S2_CopyFromBuff32       (S2_Dummy + 7)
#endif
#ifndef S2_DMACopyToBuff32
#define S2_DMACopyToBuff32      (S2_Dummy + 8)
#endif
#ifndef S2_DMACopyFromBuff32
#define S2_DMACopyFromBuff32    (S2_Dummy + 9)
#endif

#endif /* TAPPROBE_SANA2_R3_TAGS_H */
