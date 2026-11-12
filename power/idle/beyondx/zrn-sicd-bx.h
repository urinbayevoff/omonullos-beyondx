/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Between zrn-idle-bx.o and zrn-sicd-bx.o (zrn-cpuidle-bx.ko). */
#ifndef ZRN_SICD_BX_H
#define ZRN_SICD_BX_H

#include <linux/ktime.h>

void zrn_sicd_note(unsigned int core, ktime_t next);
unsigned int zrn_cpd_try(unsigned int core, bool suspended);
void zrn_cpd_done(unsigned int core, unsigned int state, int ret);
void zrn_cl2_drcg_on(void);
int zrn_sicd_init(void);
void zrn_sicd_exit(void);

#endif
