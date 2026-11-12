// SPDX-License-Identifier: GPL-2.0-only
/* Samsung PMUCAL SAVE / RESTORE / HANDLE semantics for the sleep tables. */
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
#include "sequence.h"

static bool condition(const struct zrn_seq *s, const struct zrn_seq_io *io)
{
	return (io->read(io->context, s->cond_base, s->cond_offset) &
		s->cond_mask) == s->cond_value;
}

static void write_value(const struct zrn_seq *s, zrn_u32 value,
			const struct zrn_seq_io *io)
{
	if (s->mask != (zrn_u32)~0U)
		value = (io->read(io->context, s->base, s->offset) & ~s->mask) |
			(value & s->mask);
	io->write(io->context, s->base, s->offset, value);
}

static bool value_matches(const struct zrn_seq *s, const struct zrn_seq_io *io)
{
	return (io->read(io->context, s->base, s->offset) & s->mask) == s->value;
}

/* Vendor pmucal_set/clr_bit_atomic: PMU_ALIVE offsets 0..0x3fff have
 * set (|0xc000) and clear (|0x8000) aliases; others are silently ignored. */
static void atomic_bits(const struct zrn_seq *s, zrn_u32 alias,
			const struct zrn_seq_io *io)
{
	if (s->offset <= 0x3fff)
		io->write(io->context, s->base, s->offset | alias, s->value);
}

/* Vendor pmucal_clr_pend: write back the pending bits (cond_value unused). */
static void clear_pending(const struct zrn_seq *s, const struct zrn_seq_io *io)
{
	zrn_u32 pending = io->read(io->context, s->cond_base, s->cond_offset) &
		s->cond_mask;

	io->write(io->context, s->base, s->offset, pending & s->mask);
}

int zrn_sequence_run(struct zrn_seq *seq, unsigned int size,
		     enum zrn_seq_phase phase, const struct zrn_seq_io *io,
		     unsigned int *failed)
{
	unsigned int i, retry;
	struct zrn_seq *s;
	bool conditional;

	if ((!seq && size) || !io || !io->read || !io->write || !io->delay_us ||
	    (unsigned int)phase > ZRN_HANDLE)
		return -EINVAL;
	for (i = 0; i < size; i++) {
		s = &seq[i];
		if (failed)
			*failed = i;
		if (phase == ZRN_SAVE) {
			s->valid = false;
			s->skip = false;
		}
		/* Vendor restore: a CHECK_SKIP row may cancel the next row. */
		if (phase == ZRN_RESTORE && s->skip) {
			s->skip = false;
			continue;
		}
		if (s->op == PMUCAL_CHECK_SKIP || s->op == PMUCAL_COND_CHECK_SKIP) {
			/* Restore only; next row is skipped when the value already
			 * matches or (COND_) its block is not in the expected state. */
			if (phase == ZRN_RESTORE && i + 1 < size)
				seq[i + 1].skip =
					(s->op == PMUCAL_COND_CHECK_SKIP &&
					 !condition(s, io)) || value_matches(s, io);
			else if (phase == ZRN_HANDLE)
				return -EINVAL;
			continue;
		}
		conditional = s->op == PMUCAL_COND_READ ||
			s->op == PMUCAL_COND_WRITE ||
			s->op == PMUCAL_COND_SAVE_RESTORE;
		if (conditional && !condition(s, io))
			continue;
		switch (s->op) {
		case PMUCAL_SAVE_RESTORE:
		case PMUCAL_COND_SAVE_RESTORE:
			if (phase == ZRN_SAVE) {
				s->saved = io->read(io->context, s->base, s->offset) & s->mask;
				s->valid = true;
			} else if (phase == ZRN_RESTORE && s->valid) {
				write_value(s, s->saved, io);
				s->valid = false;
			} else if (phase == ZRN_HANDLE) {
				return -EINVAL;
			}
			break;
		case PMUCAL_READ:
		case PMUCAL_COND_READ:
			if (phase != ZRN_RESTORE)
				s->saved = io->read(io->context, s->base, s->offset) & s->mask;
			break;
		case PMUCAL_WRITE:
		case PMUCAL_COND_WRITE:
			if (phase != ZRN_SAVE)
				write_value(s, s->value, io);
			break;
		case PMUCAL_WAIT:
		case PMUCAL_WAIT_TWO:
			if (phase == ZRN_SAVE)
				break;
			for (retry = 0; retry <= 1000; retry++) {
				if (value_matches(s, io))
					break;
				io->delay_us(io->context, 1);
			}
			if (retry > 1000)
				return -ETIMEDOUT;
			break;
		/* The save tables never contain these: SAVE stays write-free. */
		case PMUCAL_CLEAR_PEND:
			if (phase != ZRN_SAVE)
				clear_pending(s, io);
			break;
		case PMUCAL_SET_BIT_ATOMIC:
		case PMUCAL_CLR_BIT_ATOMIC:
			if (phase == ZRN_HANDLE)
				atomic_bits(s, s->op == PMUCAL_SET_BIT_ATOMIC ?
					    0xc000 : 0x8000, io);
			break;
		case PMUCAL_DELAY:
			if (phase == ZRN_HANDLE)
				io->delay_us(io->context, s->value);
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}
