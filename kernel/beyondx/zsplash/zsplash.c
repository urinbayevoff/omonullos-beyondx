/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * zsplash: the boot screen of OmonullOS on the Galaxy S10 5G (beyondx).
 *
 * Added for OmonullOS, November 2026. From the start of user space until
 * ztMETRO is up it keeps the kernel's and systemd's text off the screen and
 * shows what ztMETRO's own splash shows (omonullOS, "Powered by Linux"), with
 * "ztOS asosida" under it; after DELAY s the name fades out and the mark (a
 * window pane in three pieces) comes in, in the middle, with one of five
 * animations (the owner's: 2, the pane cracks open). The foot stays.
 *
 *   zsplash [VARIANT]          on /dev/fb0, until ztMETRO runs (or 120 s)
 *   zsplash --video V WIDTH    raw BGRA frames of the whole screen, scaled to
 *                              WIDTH, 30 per second, on stdout (for previews)
 *
 * On the phone: the console loglevel goes to 1 and systemd stops printing
 * its status (SIGRTMIN+21 to PID 1); both come back when zsplash ends. Only
 * the mark's square is redrawn while it moves. When /dev/fb0 changes (the
 * Exynos DRM driver takes over from simpledrm ~4 s into user space) the
 * whole screen is drawn again. Writes nothing but /dev/fb0, /dev/tty1 (hide
 * the cursor, clear, KD_GRAPHICS) and /proc/sys/kernel/printk.
 *
 * 17.11 (owner's videos): text from before (the kernel's one KERN_EMERG line)
 * came back over the picture when the DRM driver took over, and when greetd
 * switched to VT 1 fbcon painted the screen black, ~6 s before ztMETRO's
 * first frame. So the whole picture is drawn again every REDRAW s, and once
 * the DRM driver's own framebuffer has been up KD_SETTLE s tty1 goes to
 * KD_GRAPHICS (fbcon draws nothing; put back to text at the end only if
 * ztMETRO did not come, else logind owns the VT); found back in text mode, it
 * is set again and the picture redrawn. Not on simpledrm's: in graphics mode
 * fbcon does not set the new framebuffer's mode at the takeover, the panel
 * kept simpledrm's last picture (the name, 8 s) and the mark never showed.
 *
 * bxmenu4 (deferred fbcon take-over): the initramfs already shows this first
 * picture, and fbcon never sets the DRM framebuffer's mode either, as it has
 * no text. So on any framebuffer but simpledrm's zsplash pans it once
 * (FBIOPAN_DISPLAY: the fbdev helper commits its mode, what fbcon's set_par
 * did), whichever kernel it runs on.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/kd.h>

#define LW 360.0		/* the shell's logical width (scale 4 at 1440) */
#define DELAY 3.0		/* s of the name alone before the mark */
#define FADE 0.4		/* s the name takes to go */
#define MARK_AT 0.3		/* s after DELAY the mark begins */
#define MARK_S 1.45		/* the mark alone in the middle is this much bigger */
#define FPS 30
#define REDRAW 0.5		/* s between whole-screen redraws */
#define KD_SETTLE 0.5		/* s after the DRM driver's framebuffer opened */
#define RUN_MAX 120.0		/* s, then give up waiting for ztMETRO */
#define READY "/run/user/1000/ztmetro.ctl"	/* stat only: it is a FIFO */

/* the mark: a pane in perspective, cracked into three (logical units) */
static const double PART[3][5][2] = {
	{{-1, -1}, {0.15, -1}, {-0.2, 0.1}, {-1, 0.35}, {0, 0}},
	{{0.15, -1}, {1, -1}, {1, 0.5}, {-0.2, 0.1}, {0, 0}},
	{{-1, 0.35}, {-0.2, 0.1}, {1, 0.5}, {1, 1}, {-1, 1}},
};
static const int NPT[3] = {4, 4, 5};
static const double OFF[3][2] = {{-3.5, -2.5}, {3.5, -1.5}, {0, 4}};
static const double SHADE[3] = {1.0, 0.8, 0.9};
#define R 38.0			/* half the pane's side */
#define SHEAR -0.18		/* y -= 0.18 x: the pane turns away */
static const double ACC[3] = {0x1b / 255.0, 0xa1 / 255.0, 0xe2 / 255.0};

static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double out3(double t) { t = 1 - t; return 1 - t * t * t; }
static double out5(double t) { t = 1 - t; return 1 - t * t * t * t * t; }
static double out_back(double t) { double c = 1.9; t -= 1; return 1 + (c + 1) * t * t * t + c * t * t; }

/* the mark's duration per variant, s */
static const double DUR[6] = {0, 1.1, 1.3, 1.1, 1.0, 1.7};

static void centroid(int i, double *x, double *y)
{
	*x = *y = 0;
	for (int k = 0; k < NPT[i]; k++) { *x += PART[i][k][0]; *y += PART[i][k][1]; }
	*x /= NPT[i]; *y /= NPT[i];
}

static void shard_path(cairo_t *cr, int i)
{
	for (int k = 0; k < NPT[i]; k++) {
		double x = PART[i][k][0] * R, y = PART[i][k][1] * R;
		if (k == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
	}
	cairo_close_path(cr);
}

/* the mark at time t (s since it began) in logical units around (cx, cy) */
static void draw_mark(cairo_t *cr, int v, double t, double cx, double cy)
{
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, MARK_S, MARK_S);
	cairo_matrix_t sh;
	cairo_matrix_init(&sh, 1, SHEAR, 0, 1, 0, 0);
	cairo_transform(cr, &sh);
	double whole_a = 1, whole_s = 1, flash = 0, sweep = -1;
	if (v == 2) {		/* siniş: whole, then cracks open */
		whole_a = clamp01(t / 0.45);
		whole_s = 0.7 + 0.3 * out3(clamp01(t / 0.5));
		flash = t > 0.5 && t < 0.8 ? 1 - (t - 0.5) / 0.3 : 0;
	} else if (v == 5) {	/* yaltiraş: fade in, then a glint */
		whole_a = clamp01(t / 0.5);
		whole_s = 0.9 + 0.1 * out3(clamp01(t / 0.6));
		sweep = clamp01((t - 0.6) / 0.9);
	}
	cairo_scale(cr, whole_s, whole_s);
	for (int i = 0; i < 3; i++) {
		double ti, e, a = whole_a, ox = OFF[i][0], oy = OFF[i][1], rot = 0, sx = 1;
		double gx, gy;
		centroid(i, &gx, &gy);
		switch (v) {
		case 1:	/* yiğiliş: the pieces fly in and meet */
			ti = clamp01((t - 0.13 * i) / 0.75); e = out3(ti);
			{ double n = hypot(gx, gy) + 1e-6;
			  ox += gx / n * 60 * (1 - e); oy += gy / n * 60 * (1 - e); }
			rot = (i == 1 ? 25 : -25) * (1 - e) * M_PI / 180;
			a = clamp01(ti * 2.5);
			break;
		case 2:	/* siniş */
			e = t < 0.5 ? 0 : out_back(clamp01((t - 0.5) / 0.6));
			ox *= e; oy *= e;
			break;
		case 3:	/* aylaniş: each piece turns in on a hinge (Metro turnstile) */
			ti = clamp01((t - 0.12 * i) / 0.7); e = out3(ti);
			sx = cos((1 - e) * M_PI / 2 * 0.98);
			a = ti > 0 ? clamp01(ti * 3) : 0;
			break;
		case 4:	/* köteriliş: the pieces rise into place, one after another */
			ti = clamp01((t - 0.1 * i) / 0.65); e = out5(ti);
			oy += 30 * (1 - e);
			a = e;
			break;
		}
		if (a <= 0) continue;
		cairo_save(cr);
		cairo_translate(cr, ox, oy);
		if (rot != 0) {
			cairo_translate(cr, gx * R, gy * R); cairo_rotate(cr, rot); cairo_translate(cr, -gx * R, -gy * R);
		}
		if (sx != 1) {	/* hinge: the pane's left edge, a little perspective */
			cairo_translate(cr, -R, 0); cairo_scale(cr, sx, 1 + 0.12 * (1 - sx)); cairo_translate(cr, R, 0);
		}
		shard_path(cr, i);
		double k = SHADE[i];
		double r = ACC[0] * k, g = ACC[1] * k, b = ACC[2] * k;
		if (flash > 0) { r += (1 - r) * flash * 0.7; g += (1 - g) * flash * 0.7; b += (1 - b) * flash * 0.7; }
		cairo_set_source_rgba(cr, r, g, b, a);
		if (sweep > 0 && sweep < 1) {
			cairo_fill_preserve(cr);
			cairo_save(cr);
			cairo_clip(cr);
			double x0 = -2.2 * R + 4.4 * R * sweep;
			cairo_pattern_t *p = cairo_pattern_create_linear(x0 - 18, -R, x0 + 18, R);
			cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0);
			cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.75 * a);
			cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0);
			cairo_set_source(cr, p);
			cairo_paint(cr);
			cairo_pattern_destroy(p);
			cairo_restore(cr);
			cairo_new_path(cr);
		} else {
			cairo_fill(cr);
		}
		cairo_restore(cr);
	}
	cairo_restore(cr);
}

static void text(cairo_t *cr, const char *s, const char *font, double size,
		 double y, double h, double grey, double a)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d = pango_font_description_from_string(font);
	pango_font_description_set_absolute_size(d, size * PANGO_SCALE);
	pango_layout_set_font_description(l, d);
	pango_layout_set_text(l, s, -1);
	pango_layout_set_width(l, (int)(LW * PANGO_SCALE));
	pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
	int tw, th;
	pango_layout_get_pixel_size(l, &tw, &th);
	cairo_set_source_rgba(cr, grey, grey, grey, a);
	cairo_move_to(cr, 0, y + (h - th) / 2.0);
	pango_cairo_show_layout(cr, l);
	pango_font_description_free(d);
	g_object_unref(l);
}

/* as ztmetro's draw_splash: the foot 80 up ... */
static void draw_base(cairo_t *cr, double lh)
{
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	text(cr, "Powered by Linux", "Segoe WP, Selawik, Open Sans, sans-serif", 14, lh - 80, 30, 0.75, 1);
	text(cr, "ztOS asosida", "Segoe WP, Selawik, Open Sans, sans-serif", 10, lh - 54, 22, 0.47, 1);
}

/* ... and the name in the middle */
static void draw_name(cairo_t *cr, double lh, double a)
{
	text(cr, "omonullOS", "Segoe WP Light, Selawik Light, Open Sans Light, sans-serif", 56, 0, lh, 1.0, a);
}

static double name_a(double t) { return 1 - clamp01((t - DELAY) / FADE); }
static double mark_y(double lh) { return lh / 2; }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- previews ---------------------------------------------------------- */

static int video(int v, int w)
{
	int h = (int)(w * 3040.0 / 1440 + 0.5) & ~1;
	double s = w / LW, lh = h / s;
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(surf);
	double total = DELAY + MARK_AT + DUR[v] + 1.5;
	for (int f = 0; f < (int)(total * FPS); f++) {
		double t = (double)f / FPS;
		cairo_save(cr);
		cairo_scale(cr, s, s);
		draw_base(cr, lh);
		if (name_a(t) > 0) draw_name(cr, lh, name_a(t));
		if (v && t >= DELAY + MARK_AT) draw_mark(cr, v, t - DELAY - MARK_AT, LW / 2, mark_y(lh));
		cairo_restore(cr);
		cairo_surface_flush(surf);
		unsigned char *d = cairo_image_surface_get_data(surf);
		int st = cairo_image_surface_get_stride(surf);
		for (int y = 0; y < h; y++) fwrite(d + y * st, 1, w * 4, stdout);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(surf);
	return 0;
}

/* ---- the phone --------------------------------------------------------- */

/* kd_ok: not simpledrm's (see kd_graphics), from kd_at on */
static struct { int fd; unsigned char *mem; size_t len; unsigned w, h, line; dev_t id; ino_t ino; int kd_ok; double kd_at; } fb = {.fd = -1};

static void fb_close(void)
{
	if (fb.mem) munmap(fb.mem, fb.len);
	if (fb.fd >= 0) close(fb.fd);
	fb.fd = -1; fb.mem = NULL;
}

static int fb_open(void)
{
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	struct stat st;
	fb.fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb.fd < 0) return -1;
	if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fb.fd, FBIOGET_FSCREENINFO, &fi) ||
	    vi.bits_per_pixel != 32 || fstat(fb.fd, &st)) {
		fb_close();
		return -1;
	}
	fb.w = vi.xres; fb.h = vi.yres; fb.line = fi.line_length;
	fb.len = (size_t)fi.line_length * vi.yres;
	fb.id = st.st_rdev; fb.ino = st.st_ino;
	fb.mem = mmap(NULL, fb.len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
	if (fb.mem == MAP_FAILED) { fb.mem = NULL; fb_close(); return -1; }
	char name[64] = "";
	int nfd = open("/sys/class/graphics/fb0/name", O_RDONLY | O_CLOEXEC);
	if (nfd >= 0) { int n = read(nfd, name, sizeof(name) - 1); name[n > 0 ? n : 0] = 0; close(nfd); }
	fb.kd_ok = name[0] && !strstr(name, "simple");
	fb.kd_at = now() + KD_SETTLE;
	if (fb.kd_ok) {
		/* the panel shows this framebuffer only once its mode is set */
		vi.xoffset = vi.yoffset = 0;
		if (ioctl(fb.fd, FBIOPAN_DISPLAY, &vi)) {
			vi.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
			(void)!ioctl(fb.fd, FBIOPUT_VSCREENINFO, &vi);
		}
	}
	return 0;
}

/* the device node behind /dev/fb0 is another one (or gone) */
static int fb_changed(void)
{
	struct stat st;
	char name[64] = "";
	static char last[64];
	int n, fd = open("/sys/class/graphics/fb0/name", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) { n = read(fd, name, sizeof(name) - 1); name[n > 0 ? n : 0] = 0; close(fd); }
	if (stat("/dev/fb0", &st) || st.st_ino != fb.ino || st.st_rdev != fb.id) return 1;
	if (last[0] && strcmp(name, last)) { strcpy(last, name); return 1; }
	strcpy(last, name);
	return 0;
}

static void blit(cairo_surface_t *s, int x0, int y0)
{
	cairo_surface_flush(s);
	unsigned char *d = cairo_image_surface_get_data(s);
	int st = cairo_image_surface_get_stride(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	for (int y = 0; y < h && y0 + y < (int)fb.h; y++)
		memcpy(fb.mem + (size_t)(y0 + y) * fb.line + (size_t)x0 * 4, d + (size_t)y * st, (size_t)w * 4);
}

static void tty_quiet(void)
{
	int fd = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0) { (void)!write(fd, "\033[?25l\033[2J\033[H", 13); close(fd); }
	fd = open("/sys/class/graphics/fbcon/cursor_blink", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) { (void)!write(fd, "0", 1); close(fd); }
}

/* tty1 in graphics mode: fbcon leaves the screen alone. 1 when it was
 * found in text mode (just now, or put back by someone: greetd) */
static int kd_fd = -1, kd_was = -1;
static int kd_graphics(void)
{
	int m;
	if (!fb.kd_ok || now() < fb.kd_at) return 0;
	if (kd_fd < 0) kd_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (kd_fd < 0 || ioctl(kd_fd, KDGETMODE, &m)) return 0;
	if (kd_was < 0) kd_was = m;
	if (m == KD_GRAPHICS) return 0;
	(void)!ioctl(kd_fd, KDSETMODE, KD_GRAPHICS);
	return 1;
}

static char printk_was[64];

static void quiet(int on)
{
	int fd = open("/proc/sys/kernel/printk", on ? O_RDWR : O_WRONLY);
	if (fd < 0) return;
	if (on) {
		int n = read(fd, printk_was, sizeof(printk_was) - 1);
		printk_was[n > 0 ? n : 0] = 0;
		/* back to the start: sysctl ignores a write past offset 0
		 * (17.11: the level stayed 8 and the modem's text came through) */
		lseek(fd, 0, SEEK_SET);
		(void)!write(fd, "1", 1);
	} else if (printk_was[0]) {
		(void)!write(fd, printk_was, strcspn(printk_was, " \t"));
	}
	close(fd);
	kill(1, on ? SIGRTMIN + 21 : SIGRTMIN + 20);	/* systemd: status off / on */
}

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

static int phone(int v)
{
	struct stat st;
	if (!stat("/etc/zsplash.off", &st)) return 0;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	quiet(1);
	double t0 = now(), next = t0;
	cairo_surface_t *base = NULL, *named = NULL, *box = NULL, *shown = NULL;
	double full = 0;	/* when the whole screen was last drawn */
	double s = 4, lh = 760;
	int bx = 0, by = 0, bw = 0, bh = 0, fresh = 1, done = 0;
	while (!stop && now() - t0 < RUN_MAX && stat(READY, &st)) {
		if (fb.fd < 0 || fb_changed()) {
			fb_close();
			if (fb_open()) { usleep(50000); continue; }
			tty_quiet();
			if (base) cairo_surface_destroy(base);
			if (named) cairo_surface_destroy(named);
			if (box) cairo_surface_destroy(box);
			if (shown) cairo_surface_destroy(shown);
			s = fb.w / LW; lh = fb.h / s;
			base = cairo_image_surface_create(CAIRO_FORMAT_RGB24, fb.w, fb.h);
			cairo_t *cr = cairo_create(base);
			cairo_scale(cr, s, s);
			draw_base(cr, lh);
			cairo_destroy(cr);
			named = cairo_image_surface_create(CAIRO_FORMAT_RGB24, fb.w, fb.h);
			cr = cairo_create(named);
			cairo_set_source_surface(cr, base, 0, 0);
			cairo_paint(cr);
			cairo_scale(cr, s, s);
			draw_name(cr, lh, 1);
			cairo_destroy(cr);
			/* the box the name fades and the mark moves in, logical */
			bw = (int)(330 * s) & ~3;
			bh = (int)(240 * s) & ~3;
			bx = (int)(fb.w / 2.0 - bw / 2.0);
			by = (int)(mark_y(lh) * s - bh / 2.0);
			box = cairo_image_surface_create(CAIRO_FORMAT_RGB24, bw, bh);
			/* what is on the screen, whole: the box goes into it too */
			shown = cairo_image_surface_create(CAIRO_FORMAT_RGB24, fb.w, fb.h);
			cr = cairo_create(shown);
			cairo_set_source_surface(cr, now() - t0 < DELAY ? named : base, 0, 0);
			cairo_paint(cr);
			cairo_destroy(cr);
			kd_graphics();
			blit(shown, 0, 0);
			full = now();
			fresh = 1;
		}
		double t = now() - t0;
		if (t >= DELAY && (!done || fresh)) {
			double tm = t - DELAY - MARK_AT;
			cairo_t *cr = cairo_create(box);
			cairo_set_source_surface(cr, base, -bx, -by);
			cairo_paint(cr);
			if (name_a(t) > 0) {
				cairo_set_source_surface(cr, named, -bx, -by);
				cairo_paint_with_alpha(cr, name_a(t));
			}
			cairo_translate(cr, -bx, -by);
			cairo_scale(cr, s, s);
			if (v && tm >= 0) draw_mark(cr, v, tm, LW / 2, mark_y(lh));
			cairo_destroy(cr);
			blit(box, bx, by);
			cr = cairo_create(shown);
			cairo_set_source_surface(cr, box, bx, by);
			cairo_paint(cr);
			cairo_destroy(cr);
			done = tm >= DUR[v] && name_a(t) <= 0;
		}
		fresh = 0;
		if (kd_graphics() || now() - full >= REDRAW) {
			blit(shown, 0, 0);
			full = now();
		}
		next += (t >= DELAY - 0.05 && !done) ? 1.0 / FPS : 0.2;
		double w = next - now();
		if (w > 0) usleep((useconds_t)(w * 1e6)); else next = now();
	}
	quiet(0);
	if (kd_fd >= 0) {
		/* no ztMETRO: the console back for whoever comes (a rescue) */
		if (kd_was == KD_TEXT && stat(READY, &st)) (void)!ioctl(kd_fd, KDSETMODE, KD_TEXT);
		close(kd_fd);
	}
	fb_close();
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "--video")) {
		int v = atoi(argv[2]);
		return video(v >= 0 && v <= 5 ? v : 4, atoi(argv[3]) > 0 ? atoi(argv[3]) : 540);
	}
	int v = argc > 1 ? atoi(argv[1]) : 2;
	return phone(v >= 0 && v <= 5 ? v : 2);
}
