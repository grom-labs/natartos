/* SPDX-License-Identifier: AGPL-3.0-only */
volatile unsigned int natartos_health_signal;
int natartos_mod_init(void *(*lookup)(void *,const char *)){(void)lookup;natartos_health_signal=1;return 0;}
