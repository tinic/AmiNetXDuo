/* Same lifecycle probe runs in the host model and native Exec smoke.
 * Caller owns a normal serialized context. SPDX-License-Identifier: MIT */
#ifndef ANX_OBJECT_PROBE_H
#define ANX_OBJECT_PROBE_H
int anx_object_probe(void); /* zero is pass, nonzero is failing source line */
#endif
