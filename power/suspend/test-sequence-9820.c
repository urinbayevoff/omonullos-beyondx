// SPDX-License-Identifier: GPL-2.0-only
/* Host tests for the Exynos 9820 tables and the operations only they use.
 * Fake register file: no phone or physical register access. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "sequence.h"
#include "sleep-tables-9820.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define PMU 0x15860000u
#define AUD_STATUS 0x1904u
#define REGS 64

struct reg { zrn_u32 addr, value; };
struct fake {
	struct reg r[REGS];
	unsigned int n, reads, writes, delays, base_reads;
	zrn_u32 dflt, watch, last_addr, last_value;
};

static zrn_u32 *slot(struct fake *f, zrn_u32 addr)
{
	unsigned int i;

	for (i = 0; i < f->n; i++)
		if (f->r[i].addr == addr)
			return &f->r[i].value;
	return NULL;
}

static void set(struct fake *f, zrn_u32 addr, zrn_u32 value)
{
	zrn_u32 *p = slot(f, addr);

	if (!p) {
		assert(f->n < REGS);
		f->r[f->n].addr = addr;
		p = &f->r[f->n++].value;
	}
	*p = value;
}

static zrn_u32 fake_read(void *c, zrn_u32 base, zrn_u32 offset)
{
	struct fake *f = c;
	zrn_u32 *p = slot(f, base + offset);

	f->reads++;
	if (f->watch && base == f->watch)
		f->base_reads++;
	return p ? *p : f->dflt;
}

static void fake_write(void *c, zrn_u32 base, zrn_u32 offset, zrn_u32 value)
{
	struct fake *f = c;

	f->writes++;
	f->last_addr = base + offset;
	f->last_value = value;
}

static void fake_delay(void *c, unsigned int usec) { ((struct fake *)c)->delays += usec; }

static unsigned int count_op(struct zrn_seq *s, unsigned int n, enum zrn_seq_op op)
{
	unsigned int i, k = 0;

	for (i = 0; i < n; i++)
		k += s[i].op == op;
	return k;
}

int main(void)
{
	static struct fake f;
	struct zrn_seq_io io = { &f, fake_read, fake_write, fake_delay };
	struct zrn_seq item[2];
	unsigned int failed, i, valid = 0, saveable;

	assert(COUNT(enter_sleep) == 13 && COUNT(save_sleep) == 1433);
	assert(COUNT(exit_sleep) == 54 && COUNT(earlywkup_sleep) == 22);
	assert(COUNT(enter_sleep_usbl2) == 18 && COUNT(save_sleep_usbl2) == 1433);
	assert(COUNT(exit_sleep_usbl2) == 62 && COUNT(earlywkup_sleep_usbl2) == 30);

	/* SAVE never writes; every domain on (all ones) saves every row. */
	f = (struct fake) { .dflt = ~0U };
	assert(!zrn_sequence_run(save_sleep, COUNT(save_sleep), ZRN_SAVE, &io, &failed));
	assert(f.writes == 0);
	for (i = 0; i < COUNT(save_sleep); i++)
		valid += save_sleep[i].valid;
	saveable = count_op(save_sleep, COUNT(save_sleep), PMUCAL_SAVE_RESTORE) +
		count_op(save_sleep, COUNT(save_sleep), PMUCAL_COND_SAVE_RESTORE);
	assert(valid == saveable && saveable == 805);
	/* RESTORE: PLL_CON0 bit31 set (enabled) and bit29 (locked) set in the
	 * all-ones file, so both WAIT_TWO rows run and pass at once. */
	assert(!zrn_sequence_run(save_sleep, COUNT(save_sleep), ZRN_RESTORE, &io, &failed));
	assert(f.writes == valid && f.delays == 0);

	/* Every domain off (status 0): no guarded block is ever read. */
	f = (struct fake) { .dflt = ~0U, .watch = 0x18c00000 /* CMU_AUD */ };
	set(&f, PMU + AUD_STATUS, 0);
	assert(!zrn_sequence_run(save_sleep, COUNT(save_sleep), ZRN_SAVE, &io, &failed));
	assert(!zrn_sequence_run(save_sleep, COUNT(save_sleep), ZRN_RESTORE, &io, &failed));
	assert(f.base_reads == 0); /* AUD off: its CMU never touched, WAIT_TWO skipped */

	/* COND_CHECK_SKIP + WAIT_TWO, AUD on. */
	item[0] = (struct zrn_seq) { .op = PMUCAL_COND_CHECK_SKIP, .base = 0x18c00000,
		.offset = 0x120, .mask = 1u << 31, .value = 0, .cond_base = PMU,
		.cond_offset = AUD_STATUS, .cond_mask = 1, .cond_value = 1 };
	item[1] = (struct zrn_seq) { .op = PMUCAL_WAIT_TWO, .base = 0x18c00000,
		.offset = 0x120, .mask = 1u << 29, .value = 1u << 29 };
	f = (struct fake) { 0 };
	set(&f, PMU + AUD_STATUS, 1);
	set(&f, 0x18c00120, 0);	/* PLL disabled: no lock wait */
	assert(!zrn_sequence_run(item, 2, ZRN_RESTORE, &io, &failed));
	assert(f.delays == 0 && !item[1].skip);
	set(&f, 0x18c00120, 1u << 31);	/* enabled, never locks: times out */
	assert(zrn_sequence_run(item, 2, ZRN_RESTORE, &io, &failed) == -ETIMEDOUT);
	assert(failed == 1 && f.delays == 1001);
	assert(zrn_sequence_run(item, 2, ZRN_HANDLE, &io, &failed) == -EINVAL);

	/* enter_sleep: SET_BIT_ATOMIC goes to the |0xc000 alias, raw value. */
	f = (struct fake) { 0 };
	assert(!zrn_sequence_run(enter_sleep, 1, ZRN_HANDLE, &io, &failed));
	assert(f.writes == 1 && f.last_addr == PMU + 0xf044 && f.last_value == 6);
	/* CLR_BIT_ATOMIC: |0x8000 alias; offsets above 0x3fff are ignored. */
	item[0] = (struct zrn_seq) { .op = PMUCAL_CLR_BIT_ATOMIC, .base = PMU,
		.offset = 0x3044, .mask = ~0U, .value = 6 };
	assert(!zrn_sequence_run(item, 1, ZRN_HANDLE, &io, &failed));
	assert(f.last_addr == PMU + 0xb044 && f.writes == 2);
	item[0].offset = 0x4000;
	assert(!zrn_sequence_run(item, 1, ZRN_HANDLE, &io, &failed) && f.writes == 2);
	assert(!zrn_sequence_run(item, 1, ZRN_SAVE, &io, &failed) && f.writes == 2);

	/* CLEAR_PEND: write back (pending & cond_mask) & mask, no read-modify. */
	f = (struct fake) { 0 };
	set(&f, 0x15871a08, 0xffffffff);
	item[0] = (struct zrn_seq) { .op = PMUCAL_CLEAR_PEND, .base = 0x15870000,
		.offset = 0x1a0c, .mask = ~0U, .value = 0x40, .cond_base = 0x15870000,
		.cond_offset = 0x1a08, .cond_mask = 1u << 6 };
	assert(!zrn_sequence_run(item, 1, ZRN_HANDLE, &io, &failed));
	assert(f.last_addr == 0x15871a0c && f.last_value == 0x40 && f.reads == 1);
	assert(!zrn_sequence_run(item, 1, ZRN_SAVE, &io, &failed) && f.writes == 1);

	/* Masked write keeps unmasked bits and drops value bits outside mask. */
	f = (struct fake) { .dflt = 0xffffff00 };
	item[0] = (struct zrn_seq) { .op = PMUCAL_WRITE, .mask = 0x7, .value = 0xfa };
	assert(!zrn_sequence_run(item, 1, ZRN_HANDLE, &io, &failed));
	assert(f.last_value == 0xffffff02);

	/* Whole exit/earlywkup tables run in HANDLE with every bit set. */
	f = (struct fake) { .dflt = ~0U };
	assert(!zrn_sequence_run(exit_sleep, COUNT(exit_sleep), ZRN_HANDLE, &io, &failed));
	assert(!zrn_sequence_run(earlywkup_sleep, COUNT(earlywkup_sleep), ZRN_HANDLE, &io, &failed));
	/* usbl2 exit waits for the USB31DRD Q-channels (0x10a07030/702c) to idle. */
	set(&f, 0x10a07030, 0);
	set(&f, 0x10a0702c, 0);
	assert(!zrn_sequence_run(exit_sleep_usbl2, COUNT(exit_sleep_usbl2), ZRN_HANDLE, &io, &failed));
	assert(!zrn_sequence_run(earlywkup_sleep_usbl2, COUNT(earlywkup_sleep_usbl2), ZRN_HANDLE, &io, &failed));
	assert(!zrn_sequence_run(enter_sleep_usbl2, COUNT(enter_sleep_usbl2), ZRN_HANDLE, &io, &failed));
	puts("PASS 9820: table sizes, write-free save, domain guards, check-skip/wait-two, atomic aliases, clear-pend, masked write");
	return 0;
}
