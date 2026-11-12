// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztlang: see ztlang.h.
 *
 * Added for OmonullOS, October 2026.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ztlang.h"

struct zt_pair { const char *en, *tr; };

static const struct zt_pair UZ[] = {
#include "ztlang-uz.h"
};
#define N_UZ (sizeof(UZ) / sizeof(UZ[0]))

static const char *const UZ_DAYS[] = { "Yakşanba", "Duşanba", "Seşanba", "Çorşanba",
	"Payşanba", "Juma", "Şanba" };
static const char *const UZ_DAYS_SHORT[] = { "Ya", "Du", "Se", "Ço", "Pa", "Ju", "Şa" };
static const char *const UZ_MONTHS[] = { "yanvar", "fevral", "mart", "aprel", "may", "iyun",
	"iyul", "avgust", "sentabr", "oktabr", "noyabr", "dekabr" };
static const char *const UZ_MONTHS_SHORT[] = { "yan", "fev", "mar", "apr", "may", "iyn",
	"iyl", "avg", "sen", "okt", "noy", "dek" };

static enum zt_lang current = ZT_UZ;
static int loaded;
static int log_missing;	/* ZTLANG_MISSING=1: each English with no Uzbek goes to stderr */

/* the table sorted by its English, found by bsearch */
static const struct zt_pair *sorted[N_UZ];

static int by_en(const void *a, const void *b) {
	return strcmp((*(const struct zt_pair *const *)a)->en, (*(const struct zt_pair *const *)b)->en);
}

static enum zt_lang read_config(void) {
	char path[512], line[256], value[32];
	const char *home = getenv("HOME");
	enum zt_lang lang = ZT_UZ;
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", home ? home : "");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (sscanf(line, " language = %31s", value) == 1) {
			lang = strcmp(value, "en") == 0 ? ZT_EN : ZT_UZ;
		}
	}
	if (f) {
		fclose(f);
	}
	return lang;
}

static void load(void) {
	if (loaded) {
		return;
	}
	for (size_t i = 0; i < N_UZ; i++) {
		sorted[i] = &UZ[i];
	}
	qsort(sorted, N_UZ, sizeof(sorted[0]), by_en);
	current = read_config();
	log_missing = getenv("ZTLANG_MISSING") != NULL;
	loaded = 1;
}

int zt_lang_reload(void) {
	load();
	enum zt_lang was = current;
	current = read_config();
	return current != was;
}

enum zt_lang zt_lang(void) {
	load();
	return current;
}

const char *zt_lang_code(enum zt_lang lang) {
	return lang == ZT_EN ? "en" : "uz";
}

const char *zt_tr(const char *en) {
	if (!en || !*en) {
		return en;
	}
	load();
	if (current == ZT_EN) {
		return en;
	}
	struct zt_pair key = { en, NULL };
	const struct zt_pair *k = &key;
	const struct zt_pair *const *hit = bsearch(&k, sorted, N_UZ, sizeof(sorted[0]), by_en);
	if (!hit && log_missing) {
		fprintf(stderr, "ztlang: missing \"%s\"\n", en);
	}
	return hit ? (*hit)->tr : en;
}

size_t zt_strftime(char *out, size_t size, const char *format, const struct tm *tm) {
	if (zt_lang() == ZT_EN) {
		return strftime(out, size, format, tm);
	}
	/* the names put in first (with any % in them doubled), then strftime */
	char fmt[256];
	size_t n = 0;
	for (const char *p = format; *p && n + 1 < sizeof(fmt); p++) {
		const char *name = NULL;
		if (p[0] == '%' && p[1]) {
			switch (p[1]) {
			case 'A': name = UZ_DAYS[tm->tm_wday % 7]; break;
			case 'a': name = UZ_DAYS_SHORT[tm->tm_wday % 7]; break;
			case 'B': name = UZ_MONTHS[tm->tm_mon % 12]; break;
			case 'b': case 'h': name = UZ_MONTHS_SHORT[tm->tm_mon % 12]; break;
			}
		}
		if (name) {
			for (const char *q = name; *q && n + 2 < sizeof(fmt); q++) {
				if (*q == '%') {
					fmt[n++] = '%';
				}
				fmt[n++] = *q;
			}
			p++;
		} else {
			fmt[n++] = *p;
			if (p[0] == '%' && p[1] && n + 1 < sizeof(fmt)) {
				fmt[n++] = *++p;	/* %% and the rest, as they are */
			}
		}
	}
	fmt[n] = 0;
	return strftime(out, size, fmt, tm);
}
