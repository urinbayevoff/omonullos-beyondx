/*
 * ztos-passwd: changes the password of the user running it, for Settings.
 *
 * Added for OmonullOS, October 2026. Reads two lines on stdin (the current
 * password, then the new one), never from the command line, and answers
 * passwd(1)'s prompts through a pseudo terminal: the current one first, then
 * the new one, twice. Exits with passwd's status; the last line passwd wrote
 * goes to stdout, for Settings to show when it said no.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void wipe(char *s, size_t n) {
	explicit_bzero(s, n);
}

static int read_line(char *out, size_t size) {
	size_t n = 0;
	int c;
	while ((c = getchar()) != EOF && c != '\n') {
		if (n + 1 < size) out[n++] = (char)c;
	}
	out[n] = 0;
	return c == EOF && n == 0 ? -1 : 0;
}

int main(void) {
	char old[256], new[256];
	if (read_line(old, sizeof(old)) < 0 || read_line(new, sizeof(new)) < 0 || !new[0]) {
		puts("nothing to change");
		return 2;
	}
	int fd;
	pid_t pid = forkpty(&fd, NULL, NULL, NULL);
	if (pid < 0) {
		puts("could not start passwd");
		return 2;
	}
	if (pid == 0) {
		setenv("LC_ALL", "C", 1);
		execl("/usr/bin/passwd", "passwd", (char *)NULL);
		_exit(127);
	}
	/* prompts end in ": "; answer the first with the current password, the
	 * next two with the new one */
	char buf[4096], last[256] = "";
	size_t len = 0;
	int answered = 0;
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 20000) <= 0) break;	/* no prompt in 20 s: give up */
		ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0) break;	/* EIO: passwd has finished */
		len += (size_t)n;
		buf[len] = 0;
		/* keep the last line that says something */
		char *line = buf, *end;
		while ((end = strpbrk(line, "\r\n"))) {
			if (end > line) snprintf(last, sizeof(last), "%.*s", (int)(end - line), line);
			line = end + 1;
		}
		size_t rest = strlen(line);
		if (rest >= 2 && line[rest - 2] == ':' && line[rest - 1] == ' ' && answered < 3) {
			const char *answer = answered == 0 ? old : new;
			(void)!write(fd, answer, strlen(answer));
			(void)!write(fd, "\n", 1);
			answered++;
			len = 0;
		} else if (line != buf) {
			memmove(buf, line, rest + 1);
			len = rest;
		}
		if (len > sizeof(buf) / 2) len = 0;
	}
	wipe(old, sizeof(old));
	wipe(new, sizeof(new));
	close(fd);
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) status = 1 << 8;
	int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	if (rc) puts(last[0] ? last : "passwd said no");
	return rc;
}
