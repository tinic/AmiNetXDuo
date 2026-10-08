/* Host scheduling model: real Amiga types/extensions, host critical sections.
 * LP64 host layout is not Amiga ABI evidence. SPDX-License-Identifier: MIT */
#include "../../../port/threadx-amiga/inc/tx_port.h"
#undef TX_DISABLE
#undef TX_RESTORE
extern UINT anx_tx_host_disable(void);
extern void anx_tx_host_restore(UINT);
#define TX_DISABLE tx_saved_posture = anx_tx_host_disable();
#define TX_RESTORE anx_tx_host_restore(tx_saved_posture);
