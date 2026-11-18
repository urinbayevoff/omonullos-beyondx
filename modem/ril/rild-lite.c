/*
 * rild-lite: run Samsung's RIL (VENDOR libsec-ril.so) without Android's
 * framework or binder, in zmodem.sh's Android root. As stock rild it starts
 * libril's event loop and calls RIL_Init, but the RIL's answers come to us
 * (not to libril's HIDL IRadio service) and requests come from a small text
 * socket, /dev/socket/zril (see zril.py):
 *
 *   raw0 REQ               a request without data
 *   rawi REQ INT...        with an int array
 *   raws REQ STR...        with a string array ("-" = NULL, \"\" = empty)
 *   rawx REQ HEX           with bytes (OEM_HOOK_RAW 59: service mode)
 *   rawz REQ STR           with one string (SEND_USSD 29: "*102#")
 *   dial NUMBER            RIL_REQUEST_DIAL
 *
 * Every answer and unsolicited message goes to stdout and to the clients:
 *   < REQ token error n=LEN [decoded]
 *   ! UNSOL n=LEN [decoded]
 *
 * Built with the NDK for bionic (scripts/bx-ril-build.sh). Added for
 * OmonullOS, October 2026; data call answers decoded since November 2026.
 */
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define LIB	"/vendor/lib64/libsec-ril.so"
#define SOCK	"/dev/socket/zril"

/* AOSP hardware/ril/include/telephony/ril.h (multi-SIM build: socket id) */
typedef void *RIL_Token;
typedef struct {
	int version;
	void (*onRequest)(int req, void *data, size_t len, RIL_Token t, int socket_id);
	int (*onStateRequest)(int socket_id);
	int (*supports)(int req);
	void (*onCancel)(RIL_Token t);
	const char *(*getVersion)(void);
} RIL_RadioFunctions;
struct RIL_Env {
	void (*OnRequestComplete)(RIL_Token t, int e, void *resp, size_t len);
	void (*OnUnsolicitedResponse)(int unsol, const void *data, size_t len, int socket_id);
	void *(*RequestTimedCallback)(void (*cb)(void *), void *param, const struct timeval *rel);
	void (*OnRequestAck)(RIL_Token t);
};
typedef struct { char *address; int clir; void *uusInfo; } RIL_Dial;
/* SETUP_DATA_CALL (27), DATA_CALL_LIST (57), unsol DATA_CALL_LIST_CHANGED
 * (1010): one or an array of Samsung's RIL_Data_Call_Response_v12. It starts
 * as AOSP's v11 (below) but one element is 0xcaf08 bytes
 * (DataCallRespBuilder::BuildDataCallResponse in libsec-ril.so). */
#define DATA_CALL_SIZE	0xcaf08
typedef struct {
	int status, suggestedRetryTime, cid, active;
	char *type, *ifname, *addresses, *dnses, *gateways, *pcscf;
	int mtu;
} RIL_Data_Call_Response_v11;
/* SET_INITIAL_ATTACH_APN (111): AOSP's RIL_InitialAttachApn_v15, then fields
 * of Samsung's that SecRilAdaptor::CopyDataReqData reads up to offset 116 */
typedef struct {
	char *apn, *protocol, *roamingProtocol;
	int authtype;
	char *username, *password;
	int supportedTypesBitmask, bearerBitmask, modemCognitive, mtu;
	char *mvnoType, *mvnoMatchData;
	char samsung[48];
} RIL_InitialAttachApn;
_Static_assert(offsetof(RIL_InitialAttachApn, authtype) == 24 &&
	       offsetof(RIL_InitialAttachApn, username) == 32 &&
	       offsetof(RIL_InitialAttachApn, supportedTypesBitmask) == 48 &&
	       offsetof(RIL_InitialAttachApn, mtu) == 60 &&
	       offsetof(RIL_InitialAttachApn, mvnoType) == 64 &&
	       offsetof(RIL_InitialAttachApn, samsung) == 80 &&
	       sizeof(RIL_InitialAttachApn) == 128, "libsec-ril's layout");
typedef struct {
	int state, index, toa; char isMpty, isMT, als, isVoice, isVoicePrivacy;
	char *number; int numberPresentation; char *name; int namePresentation;
	void *uusInfo;
} RIL_Call;

#define TOK_MAGIC 0x7a72696cu
struct tok { unsigned magic; int req, serial; };

static const RIL_RadioFunctions *funcs;
static void *(*timed_cb)(void (*)(void *), void *, const struct timeval *);
static int clients[8], nclients;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int serial;

static void say(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	pthread_mutex_lock(&lock);
	fwrite(buf, 1, n, stdout);
	fflush(stdout);
	for (int i = 0; i < nclients; i++)
		send(clients[i], buf, n, MSG_NOSIGNAL | MSG_DONTWAIT);
	pthread_mutex_unlock(&lock);
}

static void hex(char *out, size_t size, const void *p, size_t len)
{
	const unsigned char *b = p;
	size_t o = 0;
	for (size_t i = 0; i < len && i < 256 && o + 3 < size; i++)
		o += snprintf(out + o, size - o, "%02x", b[i]);
}

/* service mode screens are text lines in binary frames */
static void text(char *out, size_t size, const void *p, size_t len)
{
	const unsigned char *b = p;
	size_t o = 0;
	for (size_t i = 0; i < len && o + 2 < size; i++)
		out[o++] = b[i] >= 0x20 && b[i] < 0x7f ? b[i] : '.';
	out[o] = 0;
}

/* the shapes of the answers we decode; anything else as hex */
static int is_strings(int req)
{
	switch (req) {
	/* not 20/21: since RIL v15 the registration states are structs
	 * (RIL_VoiceRegistrationStateResponse, RIL_DataRegistrationStateResponse),
	 * and reading them as strings crashed (2026-10-18) */
	case 22: case 51: case 98:
		return 1;
	}
	return 0;
}

static void decode(char *out, size_t size, int req, const void *p, size_t len, int unsol)
{
	size_t o = 0;
	*out = 0;
	if (!p || !len)
		return;
	if ((!unsol && is_strings(req)) || (unsol && (req == 1002 || req == 1004 || req == 1006))) {
		char *const *s = p;
		for (size_t i = 0; i < len / sizeof(char *) && o + 8 < size; i++)
			o += snprintf(out + o, size - o, " [%s]", s[i] ? s[i] : "-");
	} else if ((!unsol && (req == 11 || req == 38 || req == 39)) ||
		   (unsol && (req == 1003 || req == 1008 || req == 1009))) {
		/* one string: GET_IMSI, GET_IMEI, GET_IMEISV; SMS PDU, NITZ, ... */
		snprintf(out, size, " \"%s\"", (const char *)p);	/* SMS PDU, NITZ, ... */
	} else if (!unsol && (req == 20 || req == 21) && len >= 8) {
		/* VOICE/DATA_REGISTRATION_STATE: RIL_RegState, RIL_RadioTechnology, ... */
		const int *v = p;
		o = snprintf(out, size, " reg %d rat %d ", v[0], v[1]);
		if (o < size)
			hex(out + o, size - o, p, len > 64 ? 64 : len);
	} else if ((!unsol && req == 59) || (unsol && req == 1028)) {
		/* OEM_HOOK_RAW: hex head, then the bytes as text */
		hex(out, size / 2, p, len > 32 ? 32 : len);
		o = strlen(out);
		out[o++] = ' ';
		text(out + o, size - o, p, len);
	} else if (!unsol && req == 9) {	/* GET_CURRENT_CALLS */
		RIL_Call *const *c = p;
		for (size_t i = 0; i < len / sizeof(void *) && o + 64 < size; i++)
			o += snprintf(out + o, size - o, " {idx %d state %d %s %s}", c[i]->index,
				      c[i]->state, c[i]->isMT ? "in" : "out",
				      c[i]->number ? c[i]->number : "?");
	} else if (((!unsol && (req == 27 || req == 57)) || (unsol && req == 1010)) &&
		   len % DATA_CALL_SIZE == 0) {
		/* "{cid C active A status S retry R TYPE IFNAME addr ADDR dns DNS gw GW mtu M}";
		 * the unused slots of the list have no interface */
#define S_(x) ((x) ? (x) : "-")
		for (size_t i = 0; i < len / DATA_CALL_SIZE && o + 200 < size; i++) {
			const RIL_Data_Call_Response_v11 *d =
				(const void *)((const char *)p + i * DATA_CALL_SIZE);
			if (!d->ifname)
				continue;
			o += snprintf(out + o, size - o,
				      " {cid %d active %d status %d retry %d %s %s addr %s dns %s gw %s mtu %d}",
				      d->cid, d->active, d->status, d->suggestedRetryTime,
				      S_(d->type), d->ifname, S_(d->addresses),
				      S_(d->dnses), S_(d->gateways), d->mtu);
		}
#undef S_
	} else if (len % 4 == 0 && len <= 128) {
		const int *v = p;
		for (size_t i = 0; i < len / 4 && o + 16 < size; i++)
			o += snprintf(out + o, size - o, " %d", v[i]);
		if (o + 8 < size) {
			out[o++] = ' ';
			hex(out + o, size - o, p, len);
		}
	} else {
		out[0] = ' ';
		hex(out + 1, size - 1, p, len);
	}
}

static void on_complete(RIL_Token t, int e, void *resp, size_t len)
{
	struct tok *k = t;
	char d[4096];
	if (!k || k->magic != TOK_MAGIC) {	/* the RIL's own internal requests */
		hex(d, sizeof(d), resp, resp ? len : 0);
		say("< internal e=%d n=%zu %s", e, len, d);
		return;
	}
	k->magic = 0;
	decode(d, sizeof(d), k->req, resp, len, 0);
	say("< %d #%d e=%d n=%zu%s", k->req, k->serial, e, len, d);
	free(k);
}

static void request(int req, void *data, size_t len);

static void on_unsol(int unsol, const void *data, size_t len, int socket_id)
{
	char d[4096];
	decode(d, sizeof(d), unsol, data, len, 1);
	say("! %d n=%zu%s", unsol, len, d);
	if (unsol == 1000 && funcs->onStateRequest)	/* RADIO_STATE_CHANGED */
		say("! radio state %d", funcs->onStateRequest(0));
	if (unsol == 1001)	/* CALL_STATE_CHANGED: show the calls */
		request(9, NULL, 0);
	(void)socket_id;
}

static void on_ack(RIL_Token t)
{
	struct tok *k = t;
	if (k && k->magic == TOK_MAGIC)
		say("< ack #%d", k->serial);
}

/* without libril's event loop: a thread per timed callback */
struct timed { void (*cb)(void *); void *param; struct timeval rel; };
static void *timed_thread(void *a)
{
	struct timed *t = a;
	usleep(t->rel.tv_sec * 1000000 + t->rel.tv_usec);
	t->cb(t->param);
	free(t);
	return NULL;
}
static void *own_timed(void (*cb)(void *), void *param, const struct timeval *rel)
{
	struct timed *t = calloc(1, sizeof(*t));
	pthread_t th;
	t->cb = cb;
	t->param = param;
	if (rel)
		t->rel = *rel;
	pthread_create(&th, NULL, timed_thread, t);
	pthread_detach(th);
	return t;
}
static void *env_timed(void (*cb)(void *), void *param, const struct timeval *rel)
{
	return timed_cb(cb, param, rel);
}

static struct RIL_Env env = { on_complete, on_unsol, env_timed, on_ack };

static void request(int req, void *data, size_t len)
{
	struct tok *k = malloc(sizeof(*k));
	k->magic = TOK_MAGIC;
	k->req = req;
	k->serial = __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED);
	say("> %d #%d n=%zu", req, k->serial, len);
	funcs->onRequest(req, data, len, k, 0);
}

static void command(char *line)
{
	char *argv[32], *save;
	int argc = 0;
	for (char *a = strtok_r(line, " \t\r\n", &save); a && argc < 32; a = strtok_r(NULL, " \t\r\n", &save))
		argv[argc++] = a;
	if (argc < 1)
		return;
	if (!strcmp(argv[0], "dial") && argc > 1) {
		RIL_Dial d = { argv[1], 0, NULL };
		request(10, &d, sizeof(d));
		return;
	}
	if (!strcmp(argv[0], "attach") && argc == 6) {	/* attach APN PROTOCOL USER PASSWORD AUTH */
#define EMPTY_(x) (!strcmp((x), "\"\"") ? (char *)"" : (x))
		RIL_InitialAttachApn a = {
			.apn = EMPTY_(argv[1]), .protocol = argv[2], .roamingProtocol = argv[2],
			.authtype = atoi(argv[5]), .username = EMPTY_(argv[3]),
			.password = EMPTY_(argv[4]), .supportedTypesBitmask = 1,	/* default */
			.mvnoType = (char *)"", .mvnoMatchData = (char *)"",
		};
#undef EMPTY_
		request(111, &a, sizeof(a));
		return;
	}
	if (argc < 2) {
		say("? usage: raw0|rawi|raws|rawx|rawz REQ ..., dial NUMBER, attach APN PROTOCOL USER PASSWORD AUTH");
		return;
	}
	int req = atoi(argv[1]), n = argc - 2;
	if (!strcmp(argv[0], "raw0")) {
		request(req, NULL, 0);
	} else if (!strcmp(argv[0], "rawi")) {
		int v[30];
		for (int i = 0; i < n; i++)
			v[i] = strtol(argv[i + 2], NULL, 0);
		request(req, v, n * sizeof(int));
	} else if (!strcmp(argv[0], "raws")) {
		char *v[30];
		for (int i = 0; i < n; i++)
			v[i] = !strcmp(argv[i + 2], "-") ? NULL :
			       !strcmp(argv[i + 2], "\"\"") ? (char *)"" : argv[i + 2];
		request(req, v, n * sizeof(char *));
	} else if (!strcmp(argv[0], "rawz") && n == 1) {
		request(req, argv[2], strlen(argv[2]) + 1);
	} else if (!strcmp(argv[0], "rawx") && n == 1) {
		unsigned char v[512];
		size_t l = strlen(argv[2]) / 2;
		if (l > sizeof(v))
			l = sizeof(v);
		for (size_t i = 0; i < l; i++)
			sscanf(argv[2] + 2 * i, "%2hhx", &v[i]);
		request(req, v, l);
	} else {
		say("? unknown command %s", argv[0]);
	}
}

static void serve(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	strncpy(a.sun_path, SOCK, sizeof(a.sun_path) - 1);
	unlink(SOCK);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 4)) {
		perror(SOCK);
		exit(1);
	}
	chmod(SOCK, 0660);
	char bufs[8][1024];
	size_t used[8] = { 0 };
	for (;;) {
		struct pollfd p[9] = { { s, POLLIN, 0 } };
		pthread_mutex_lock(&lock);
		int n = nclients;
		for (int i = 0; i < n; i++)
			p[i + 1] = (struct pollfd){ clients[i], POLLIN, 0 };
		pthread_mutex_unlock(&lock);
		if (poll(p, n + 1, -1) < 0)
			continue;
		if ((p[0].revents & POLLIN) && n < 8) {
			int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
			pthread_mutex_lock(&lock);
			if (c >= 0) {
				used[nclients] = 0;
				clients[nclients++] = c;
			}
			pthread_mutex_unlock(&lock);
		}
		for (int i = n - 1; i >= 0; i--) {
			if (!p[i + 1].revents)
				continue;
			ssize_t r = read(clients[i], bufs[i] + used[i], sizeof(bufs[i]) - 1 - used[i]);
			if (r <= 0) {
				pthread_mutex_lock(&lock);
				close(clients[i]);
				clients[i] = clients[--nclients];
				memcpy(bufs[i], bufs[nclients], used[nclients]);
				used[i] = used[nclients];
				pthread_mutex_unlock(&lock);
				continue;
			}
			used[i] += r;
			bufs[i][used[i]] = 0;
			char *nl;
			while ((nl = strchr(bufs[i], '\n'))) {
				*nl = 0;
				command(bufs[i]);
				used[i] -= nl + 1 - bufs[i];
				memmove(bufs[i], nl + 1, used[i] + 1);
			}
			if (used[i] == sizeof(bufs[i]) - 1)
				used[i] = 0;
		}
	}
}

int main(int argc, char **argv)
{
	/* stock rild: rilArgv = { argv[0], "-c", clientId } (vendor.rild.libargs empty) */
	char *rargv[] = { "rild", "-c", "1", NULL };

	setvbuf(stdout, NULL, _IOLBF, 0);
	/* libril (libril_sem.so, a dependency of libsec-ril) has the event loop */
	void *libril = dlopen("libril_sem.so", RTLD_NOW | RTLD_GLOBAL);
	void (*start_loop)(void) = libril ? dlsym(libril, "RIL_startEventLoop") : NULL;
	timed_cb = libril ? dlsym(libril, "RIL_requestTimedCallback") : NULL;
	if (start_loop && timed_cb && !(argc > 1 && !strcmp(argv[1], "--own-loop"))) {
		start_loop();
		say("# libril event loop started");
	} else {
		timed_cb = own_timed;
		say("# own timed callbacks (libril: %s)", libril ? "loaded" : dlerror());
	}
	void *lib = dlopen(LIB, RTLD_NOW);
	if (!lib) {
		say("# dlopen: %s", dlerror());
		return 1;
	}
	const RIL_RadioFunctions *(*init)(const struct RIL_Env *, int, char **) = dlsym(lib, "RIL_Init");
	if (!init) {
		say("# no RIL_Init");
		return 1;
	}
	say("# RIL_Init...");
	funcs = init(&env, 3, rargv);
	if (!funcs) {
		say("# RIL_Init returned NULL");
		return 1;
	}
	say("# RIL version %d, %s, radio state %d", funcs->version,
	    funcs->getVersion ? funcs->getVersion() : "?",
	    funcs->onStateRequest ? funcs->onStateRequest(0) : -1);
	serve();
}
