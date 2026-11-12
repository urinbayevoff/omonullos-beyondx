// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztlang: the language the shell and our own apps speak.
 *
 * Added for OmonullOS, October 2026: Uzbek (Latin) beside English.
 *
 * The English text is the key: ZT_("settings") is "sozlamalar" when the
 * language is Uzbek, and "settings" otherwise, or when there is no
 * translation. The drawing functions (zmc_text, the shell's draw_text, and
 * their width measures) pass whatever they draw through it, so a plain
 * string literal needs nothing else. A string put together with snprintf
 * passes its format through ZT_() where it is made.
 *
 * The language is "language = uz | en" in ~/.config/ztmetro/config; when
 * the line is not there it is Uzbek.
 *
 * ZTLANG_MISSING=1 in the environment: each English that has no Uzbek is
 * written to stderr as it is drawn, to find what the table lacks (a page
 * drawn with --render, then sort -u).
 */
#ifndef ZTLANG_H
#define ZTLANG_H

#include <stddef.h>
#include <time.h>

enum zt_lang { ZT_EN, ZT_UZ };

/* English in, the translation out (or the English, if there is none) */
const char *zt_tr(const char *en) __attribute__((format_arg(1)));
#define ZT_(s) zt_tr(s)

enum zt_lang zt_lang(void);
const char *zt_lang_code(enum zt_lang lang);	/* "en", "uz" */
/* Read the config again; true when the language changed. */
int zt_lang_reload(void);

/* strftime, but %A %a %B %b in the language */
size_t zt_strftime(char *out, size_t size, const char *format, const struct tm *tm);

#endif
