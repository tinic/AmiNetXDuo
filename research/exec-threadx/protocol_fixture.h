/* Disposable protocol-test PRNG, not production entropy. SPDX-License-Identifier: MIT */
#ifndef ANX_PROTOCOL_FIXTURE_H
#define ANX_PROTOCOL_FIXTURE_H
int anx_research_rand(void);
void anx_research_srand(unsigned int);
#define NX_RAND anx_research_rand
#define NX_SRAND anx_research_srand
#endif
