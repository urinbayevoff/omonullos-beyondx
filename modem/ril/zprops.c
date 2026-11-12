/*
 * zprops: the two Android services Samsung's RIL (libsec-ril.so) and cbd
 * expect, for the small Android root of zmodem.sh, without Android's init:
 *
 *   - system properties: /dev/__properties__ as a single "pre-split"
 *     property area (bionic's layout, read by bionic's own code), filled from
 *     the given build.prop-style files, and the property service on
 *     /dev/socket/property_service (protocol 2) for setprop;
 *     persist.* values are kept in PERSIST and loaded again at start
 *   - a log sink on /dev/socket/logdw: liblog's messages go to stdout
 *
 *   zprops [-p PERSIST] FILE...    (later files override earlier ones)
 *
 * Built for Arch (static), not bionic: it runs outside the Android root;
 * /dev is shared with it. Added for OmonullOS, October 2026.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <linux/futex.h>
#include <time.h>
#include <unistd.h>

#define PROP_FILE	"/dev/__properties__"
#define PROP_SOCK	"/dev/socket/property_service"
#define LOG_SOCK	"/dev/socket/logdw"
#define PA_SIZE		(128 * 1024)
#define PROP_VALUE_MAX	92

/* bionic system_properties/prop_area.h, prop_info.h (Android 10+) */
struct prop_area {
	uint32_t bytes_used;
	_Atomic uint32_t serial;
	uint32_t magic;		/* 0x504f5250 */
	uint32_t version;	/* 0xfc6ed0ab */
	uint32_t reserved[28];
	char data[];
};
struct prop_bt {
	uint32_t namelen;
	_Atomic uint32_t prop, left, right, children;
	char name[];
};
struct prop_info {
	_Atomic uint32_t serial;
	char value[PROP_VALUE_MAX];
	char name[];
};

static struct prop_area *pa;
static const char *persist_file;

static void *obj(uint32_t off) { return pa->data + off; }

static void *alloc(size_t size, uint32_t *off)
{
	size = (size + 3) & ~3UL;
	if (pa->bytes_used + size > PA_SIZE - sizeof(*pa))
		return NULL;
	*off = pa->bytes_used;
	pa->bytes_used += size;
	return pa->data + *off;
}

static struct prop_bt *new_bt(const char *name, uint32_t len, uint32_t *off)
{
	struct prop_bt *bt = alloc(sizeof(*bt) + len + 1, off);
	if (bt) {
		memcpy(bt->name, name, len);
		bt->name[len] = 0;
		bt->namelen = len;
	}
	return bt;
}

static int cmp(const char *a, uint32_t al, const char *b, uint32_t bl)
{
	return al < bl ? -1 : al > bl ? 1 : strncmp(a, b, al);
}

/* bionic prop_area::find_prop_bt: a binary tree per trie level */
static struct prop_bt *find_bt(struct prop_bt *bt, const char *name, uint32_t len)
{
	for (;;) {
		int r = cmp(name, len, bt->name, bt->namelen);
		if (!r)
			return bt;
		_Atomic uint32_t *link = r < 0 ? &bt->left : &bt->right;
		uint32_t off = atomic_load(link);
		if (off) {
			bt = obj(off);
			continue;
		}
		struct prop_bt *n = new_bt(name, len, &off);
		if (n)
			atomic_store_explicit(link, off, memory_order_release);
		return n;
	}
}

/* bionic prop_area::find_property with alloc_if_needed */
static struct prop_info *find(const char *name, int create, const char *value)
{
	struct prop_bt *cur = obj(0);
	const char *rest = name;
	uint32_t off;

	for (;;) {
		const char *sep = strchr(rest, '.');
		uint32_t len = sep ? (uint32_t)(sep - rest) : strlen(rest);
		struct prop_bt *root;
		if (!len)
			return NULL;
		off = atomic_load(&cur->children);
		if (off)
			root = obj(off);
		else if (create && (root = new_bt(rest, len, &off)))
			atomic_store_explicit(&cur->children, off, memory_order_release);
		else
			return NULL;
		if (!(cur = create ? find_bt(root, rest, len) : root))
			return NULL;
		if (!create) {	/* lookup only: walk without allocating */
			for (;;) {
				int r = cmp(rest, len, cur->name, cur->namelen);
				if (!r)
					break;
				off = atomic_load(r < 0 ? &cur->left : &cur->right);
				if (!off)
					return NULL;
				cur = obj(off);
			}
		}
		if (!sep)
			break;
		rest = sep + 1;
	}
	off = atomic_load(&cur->prop);
	if (off)
		return obj(off);
	if (!create)
		return NULL;
	size_t nl = strlen(name), vl = strlen(value);
	struct prop_info *pi = alloc(sizeof(*pi) + nl + 1, &off);
	if (!pi)
		return NULL;
	memcpy(pi->name, name, nl + 1);
	memcpy(pi->value, value, vl + 1);
	atomic_store_explicit(&pi->serial, vl << 24, memory_order_relaxed);
	atomic_store_explicit(&cur->prop, off, memory_order_release);
	return pi;
}

static void wake(_Atomic uint32_t *p)
{
	syscall(SYS_futex, p, FUTEX_WAKE, INT32_MAX, NULL, NULL, 0);
}

/* bionic SystemProperties::Add / Update; 0 on success, else an error code */
static int set(const char *name, const char *value)
{
	size_t vl = strlen(value);
	if (vl >= PROP_VALUE_MAX)
		return 0x14;	/* PROP_ERROR_INVALID_VALUE */
	struct prop_info *pi = find(name, 0, NULL);
	if (!pi) {
		if (!find(name, 1, value))
			return 0x18;	/* PROP_ERROR_SET_FAILED */
	} else {
		uint32_t serial = atomic_load_explicit(&pi->serial, memory_order_relaxed);
		char *backup = pa->data + sizeof(struct prop_bt);
		memcpy(backup, pi->value, (serial >> 24) + 1);
		atomic_thread_fence(memory_order_release);
		serial |= 1;
		atomic_store_explicit(&pi->serial, serial, memory_order_relaxed);
		memcpy(pi->value, value, vl + 1);
		atomic_thread_fence(memory_order_release);
		atomic_store_explicit(&pi->serial, (vl << 24) | ((serial + 1) & 0xffffff),
				      memory_order_relaxed);
		wake(&pi->serial);
	}
	atomic_store_explicit(&pa->serial, atomic_load(&pa->serial) + 1, memory_order_release);
	wake(&pa->serial);
	return 0;
}

static void load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	if (!f) {
		fprintf(stderr, "zprops: %s: %s\n", path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *k = line + strspn(line, " \t"), *v = strchr(k, '=');
		if (*k == '#' || !v)
			continue;
		*v++ = 0;
		k[strcspn(k, " \t")] = 0;
		v[strcspn(v, "\r\n")] = 0;
		if (*k && set(k, v))
			fprintf(stderr, "zprops: %s: not set (value too long?)\n", k);
	}
	fclose(f);
}

static void save_persist(const char *name, const char *value)
{
	/* the whole file again: few persist.* values, set rarely */
	char tmp[256], line[512];
	snprintf(tmp, sizeof(tmp), "%s.tmp", persist_file);
	FILE *in = fopen(persist_file, "r"), *out = fopen(tmp, "w");
	if (!out)
		return;
	size_t nl = strlen(name);
	while (in && fgets(line, sizeof(line), in))
		if (strncmp(line, name, nl) || line[nl] != '=')
			fputs(line, out);
	fprintf(out, "%s=%s\n", name, value);
	if (in)
		fclose(in);
	fflush(out);
	fsync(fileno(out));
	fclose(out);
	rename(tmp, persist_file);
}

static int read_all(int fd, void *buf, size_t len)
{
	for (size_t got = 0; got < len;) {
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 2000) <= 0)
			return -1;
		ssize_t n = read(fd, (char *)buf + got, len - got);
		if (n <= 0)
			return -1;
		got += n;
	}
	return 0;
}

static int read_str(int fd, char *buf, size_t size)
{
	uint32_t len;
	if (read_all(fd, &len, 4) || len >= size || read_all(fd, buf, len))
		return -1;
	buf[len] = 0;
	return 0;
}

/* bionic __system_property_set, protocol 2 (PROP_MSG_SETPROP2) */
static void serve(int c)
{
	uint32_t cmd, res = 0x18;
	char name[256], value[256];

	if (read_all(c, &cmd, 4) || cmd != 0x20001 ||
	    read_str(c, name, sizeof(name)) || read_str(c, value, sizeof(value)))
		goto out;
	if (!strncmp(name, "ro.", 3) && find(name, 0, NULL))
		res = 0x0b;	/* PROP_ERROR_READ_ONLY_PROPERTY */
	else if (!strncmp(name, "ctl.", 4))
		res = 0;	/* no services to start or stop */
	else if (!(res = set(name, value)) && persist_file && !strncmp(name, "persist.", 8))
		save_persist(name, value);
	printf("setprop %s=%s%s\n", name, value, res ? " (refused)" : "");
	fflush(stdout);
	if (write(c, &res, 4) != 4)
		perror("property_service");
out:
	close(c);
}

/* liblog's logd writer: android_log_header_t (11 bytes), then priority, tag, message */
static void logline(int s)
{
	unsigned char buf[5000];
	ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
	if (n < 13 || buf[0] == 2 /* LOG_ID_EVENTS */)
		return;
	buf[n] = 0;
	int prio = buf[11];
	const char *tag = (char *)buf + 12, *msg = tag + strlen(tag) + 1;
	if (msg >= (char *)buf + n)
		return;
	printf("%c/%s(%u): %s\n", "??VDIWEF"[prio & 7], tag, buf[1] | buf[2] << 8, msg);
	fflush(stdout);
}

static int listen_on(const char *path, int type)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	unlink(path);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) ||
	    (type == SOCK_STREAM && listen(s, 8))) {
		perror(path);
		exit(1);
	}
	chmod(path, 0666);
	return s;
}

int main(int argc, char **argv)
{
	int i = 1, fd;

	if (argc > 2 && !strcmp(argv[1], "-p")) {
		persist_file = argv[2];
		i = 3;
	}
	unlink(PROP_FILE);
	fd = open(PROP_FILE, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
	if (fd < 0 || ftruncate(fd, PA_SIZE)) {
		perror(PROP_FILE);
		return 1;
	}
	pa = mmap(NULL, PA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pa == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	pa->magic = 0x504f5250;
	pa->version = 0xfc6ed0ab;
	/* the root node, then the dirty backup area readers use during updates */
	pa->bytes_used = sizeof(struct prop_bt) + ((PROP_VALUE_MAX + 3) & ~3);

	for (; i < argc; i++)
		load(argv[i]);
	if (persist_file)
		load(persist_file);
	set("ro.property_service.version", "2");
	printf("zprops: %u bytes of %d used\n", pa->bytes_used, PA_SIZE);

	mkdir("/dev/socket", 0755);
	struct pollfd p[2] = {
		{ listen_on(PROP_SOCK, SOCK_STREAM), POLLIN, 0 },
		{ listen_on(LOG_SOCK, SOCK_DGRAM), POLLIN, 0 },
	};
	fflush(stdout);
	for (;;) {
		if (poll(p, 2, -1) < 0)
			continue;
		if (p[0].revents & POLLIN) {
			int c = accept4(p[0].fd, NULL, NULL, SOCK_CLOEXEC);
			if (c >= 0)
				serve(c);
		}
		if (p[1].revents & POLLIN)
			logline(p[1].fd);
	}
}
