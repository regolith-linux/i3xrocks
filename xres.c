/*
 * xres.c - X resources lookup
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xcb_xrm.h>

#include "log.h"
#include "xres.h"

/*
 * The resources are fetched by a helper process, so that an X server which
 * accepts the connection but never answers cannot hold the bar: past the
 * startup delay, every resource is considered unset. The helper then keeps
 * looking for a display, and reports the resources of the first one holding
 * some, which the bar adopts.
 *
 * Each attempt runs in a child process of the helper and writes its result
 * to a pipe, as a type byte followed by the resources, if any, and a null
 * byte. The helper writes the results it reports to the bar the same way.
 */

/* Time given to the X server to provide its resources, from startup */
#define XRES_STARTUP_MSEC	1000

/* Without resources, look for a display again this often */
#define XRES_RETRY_MSEC		2000

/* Time after which an attempt gives up on a silent server */
#define XRES_ATTEMPT_SEC	10

/* Attempts held by silent servers at the same time, at most */
#define XRES_ATTEMPTS_MAX	(XRES_ATTEMPT_SEC * 1000 / XRES_RETRY_MSEC + 1)

#define XRES_NONE	'N'	/* no usable display */
#define XRES_STORE	'S'	/* the RESOURCE_MANAGER property of the server */
#define XRES_FILES	'F'	/* the resource files, the server holds none */

/* Messages read from a pipe */
struct xres_msgs {
	int fd;
	char *buf;
	size_t len;
	size_t used;	/* by the message returned last */
};

static struct {
	struct timespec deadline;
	bool queried;
	struct xres_msgs helper;
	xcb_xrm_database_t *database;
} xres = {
	.helper.fd = -1,
};

static void xres_add_msec(struct timespec *ts, long msec)
{
	ts->tv_sec += msec / 1000;
	ts->tv_nsec += (msec % 1000) * 1000000;
	if (ts->tv_nsec >= 1000000000) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000;
	}
}

/* Milliseconds until ts, rounded up, or 0 if reached */
static int xres_msec_left(const struct timespec *ts)
{
	struct timespec now;
	long long nsec;

	clock_gettime(CLOCK_MONOTONIC, &now);

	nsec = (ts->tv_sec - now.tv_sec) * 1000000000LL + ts->tv_nsec - now.tv_nsec;
	if (nsec <= 0)
		return 0;

	return (nsec + 999999) / 1000000;
}

int xres_init(void)
{
	if (clock_gettime(CLOCK_MONOTONIC, &xres.deadline))
		return -errno;

	xres_add_msec(&xres.deadline, XRES_STARTUP_MSEC);

	return 0;
}

static int xres_write(int fd, const char *buf, size_t len)
{
	ssize_t n;

	while (len) {
		n = write(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		buf += n;
		len -= n;
	}

	return 0;
}

static int xres_send(int fd, char type, const char *str)
{
	int err;

	err = xres_write(fd, &type, 1);
	if (!err && str)
		err = xres_write(fd, str, strlen(str));
	if (!err)
		err = xres_write(fd, "", 1);

	return err;
}

/*
 * Read from a pipe; return 1 and point to the next message, without its
 * terminating null byte, 0 at the end, or a negative error code (-EAGAIN
 * without a complete message yet). The message is valid until the next call.
 */
static int xres_recv(struct xres_msgs *msgs, char **msg)
{
	char buf[4096];
	char *end, *tmp;
	ssize_t n;

	/* Drop the message returned last time */
	if (msgs->used) {
		msgs->len -= msgs->used;
		memmove(msgs->buf, msgs->buf + msgs->used, msgs->len);
		msgs->used = 0;
	}

	for (;;) {
		end = msgs->len ? memchr(msgs->buf, '\0', msgs->len) : NULL;
		if (end) {
			*msg = msgs->buf;
			msgs->used = end + 1 - msgs->buf;
			return 1;
		}

		n = read(msgs->fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		if (n == 0)
			return 0;

		tmp = realloc(msgs->buf, msgs->len + n);
		if (!tmp)
			return -ENOMEM;

		memcpy(tmp + msgs->len, buf, n);
		msgs->buf = tmp;
		msgs->len += n;
	}
}

/* Fetch the resources and write them to fd, in a child process */
static void xres_attempt(int fd)
{
	xcb_xrm_database_t *database = NULL;
	xcb_connection_t *conn;
	xcb_screen_t *screen;
	char type = XRES_NONE;
	char *str = NULL;

	/* Give up on a silent server */
	signal(SIGALRM, SIG_DFL);
	alarm(XRES_ATTEMPT_SEC);

	/* Without a server, don't look for resource files either */
	conn = xcb_connect(NULL, NULL);
	if (!xcb_connection_has_error(conn)) {
		/* The screen used by xcb_xrm_database_from_default() */
		screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
		if (screen) {
			database = xcb_xrm_database_from_resource_manager(conn, screen);
			type = XRES_STORE;
		}

		if (!database && !xcb_connection_has_error(conn)) {
			database = xcb_xrm_database_from_default(conn);
			type = XRES_FILES;
		}

		if (database) {
			/* NULL if empty */
			str = xcb_xrm_database_to_string(database);
			xcb_xrm_database_free(database);
		} else {
			type = XRES_NONE;
		}
	}
	xcb_disconnect(conn);

	xres_send(fd, type, str);
	free(str);
}

struct xres_attempt {
	struct xres_msgs msgs;
	pid_t pid;
	bool first;
};

static int xres_attempt_spawn(struct xres_attempt *attempt, int out)
{
	int fds[2];
	pid_t pid;

	if (pipe(fds))
		return -errno;

	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -errno;
	}

	if (pid == 0) {
		/* The bar sees the end of the helper as soon as it exits */
		close(out);
		close(fds[0]);
		xres_attempt(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);

	attempt->msgs = (struct xres_msgs) { .fd = fds[0] };
	attempt->pid = pid;

	return 0;
}

static void xres_attempt_end(struct xres_attempt *attempt, bool kill_it)
{
	if (kill_it)
		kill(attempt->pid, SIGKILL);

	while (waitpid(attempt->pid, NULL, 0) < 0 && errno == EINTR)
		continue;

	close(attempt->msgs.fd);
	free(attempt->msgs.buf);
}

/* Set up the helper process, apart from the bar */
static void xres_helper_init(void)
{
	sigset_t set;
	int null;
	int sig;

	/* Hold neither the output of the bar nor its input */
	null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		close(null);
	}

	/* Not to be taken for the bar by pgrep -x or pkill -x */
	prctl(PR_SET_NAME, "i3xrocks-xres");

	/* Survive the signals sent to the bar ("pkill -RTMIN+1 i3xrocks") */
	signal(SIGHUP, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	for (sig = SIGRTMIN; sig <= SIGRTMAX; sig++)
		signal(sig, SIG_IGN);

	/* Writing once the bar is gone fails, the attempts are then cleaned up */
	signal(SIGPIPE, SIG_IGN);

	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL);
}

/*
 * Report to the bar the result of the first attempt, if it comes within the
 * startup delay, then the first resources held by a server, and exit.
 */
static void xres_helper(int out)
{
	static char none[] = { XRES_NONE, '\0' };
	struct xres_attempt attempts[XRES_ATTEMPTS_MAX];
	struct pollfd pfds[1 + XRES_ATTEMPTS_MAX];
	struct timespec next;
	bool first = true;
	bool done = false;
	int i, n = 0;
	char *msg;
	int err;

	xres_helper_init();

	clock_gettime(CLOCK_MONOTONIC, &next);

	while (!done) {
		if (n < XRES_ATTEMPTS_MAX && xres_msec_left(&next) == 0) {
			attempts[n].first = first;
			if (xres_attempt_spawn(&attempts[n], out) == 0) {
				first = false;
				n++;
			}

			clock_gettime(CLOCK_MONOTONIC, &next);
			xres_add_msec(&next, XRES_RETRY_MSEC);
		}

		/* Errors on the pipe tell that the bar is gone */
		pfds[0].fd = out;
		pfds[0].events = 0;
		for (i = 0; i < n; i++) {
			pfds[1 + i].fd = attempts[i].msgs.fd;
			pfds[1 + i].events = POLLIN;
		}

		/* With every attempt held, wait for one to end (alarm) */
		err = poll(pfds, 1 + n, n < XRES_ATTEMPTS_MAX ? xres_msec_left(&next) : -1);
		if (err < 0 && errno != EINTR)
			break;
		if (err <= 0)
			continue;

		if (pfds[0].revents)
			break;

		for (i = n - 1; i >= 0; i--) {
			if (!pfds[1 + i].revents)
				continue;

			err = xres_recv(&attempts[i].msgs, &msg);
			if (err == -EAGAIN)
				continue;

			/* Killed (by its alarm) without a result */
			if (err <= 0)
				msg = none;

			if (attempts[i].first && xres_msec_left(&xres.deadline) > 0) {
				xres_send(out, msg[0], msg + 1);
				done = msg[0] != XRES_NONE;
			} else if (msg[0] == XRES_STORE) {
				/* A display found later on, or after a slow answer */
				xres_send(out, msg[0], msg + 1);
				done = true;
			}

			xres_attempt_end(&attempts[i], false);
			attempts[i] = attempts[--n];
		}
	}

	for (i = 0; i < n; i++)
		xres_attempt_end(&attempts[i], true);
}

/* Start the helper, detached so that the bar never waits for it */
static int xres_spawn(void)
{
	int fds[2];
	pid_t pid;

	if (pipe(fds))
		return -errno;

	if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) ||
	    fcntl(fds[1], F_SETFD, FD_CLOEXEC) ||
	    fcntl(fds[0], F_SETFL, O_NONBLOCK))
		goto err;

	pid = fork();
	if (pid < 0)
		goto err;

	if (pid == 0) {
		close(fds[0]);

		if (fork() == 0)
			xres_helper(fds[1]);

		_exit(0);
	}

	close(fds[1]);

	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		continue;

	xres.helper.fd = fds[0];

	return 0;
err:
	close(fds[0]);
	close(fds[1]);

	return -errno;
}

/* Use the resources of a message from the helper, if any */
static bool xres_use(const char *msg)
{
	xcb_xrm_database_t *database;

	if (msg[0] != XRES_STORE && msg[0] != XRES_FILES)
		return false;

	database = xcb_xrm_database_from_string(msg + 1);
	if (!database) {
		error("failed to parse the Xresources");
		return false;
	}

	if (xres.database)
		xcb_xrm_database_free(xres.database);

	xres.database = database;

	return true;
}

/* Wait for the first result of the helper, until the end of the startup delay */
static void xres_query(void)
{
	const char *display = getenv("DISPLAY");
	struct pollfd pfd;
	char *msg;
	int err;

	/* No display can show up later on either */
	if (!display || *display == '\0') {
		xres.queried = true;
		return;
	}

	err = xres_spawn();
	xres.queried = true;
	if (err) {
		error("failed to query Xresources: %s", strerror(-err));
		return;
	}

	pfd.fd = xres.helper.fd;
	pfd.events = POLLIN;

	for (;;) {
		err = poll(&pfd, 1, xres_msec_left(&xres.deadline));
		if (err < 0 && errno == EINTR)
			continue;

		if (err <= 0) {
			trace("Xresources unavailable within the startup delay");
			return;
		}

		err = xres_recv(&xres.helper, &msg);
		if (err == -EAGAIN)
			continue;

		/* Resources found, or the helper is gone */
		if (err <= 0 || xres_use(msg))
			break;

		/* No display, the helper keeps looking */
		trace("Xresources unavailable, looking for a display");
		return;
	}

	close(xres.helper.fd);
	xres.helper.fd = -1;
}

char *xres_get(const char *name)
{
	char *value = NULL;

	if (!xres.queried)
		xres_query();

	if (!xres.database)
		return NULL;

	if (xcb_xrm_resource_get_string(xres.database, name, NULL, &value) < 0 ||
	    !value || *value == '\0') {
		free(value);
		return NULL;
	}

	return value;
}

int xres_watch(void)
{
	return xres.helper.fd;
}

bool xres_update(void)
{
	bool adopted = false;
	char *msg;
	int err;

	if (xres.helper.fd < 0)
		return false;

	while ((err = xres_recv(&xres.helper, &msg)) == 1)
		adopted |= xres_use(msg);

	if (err != -EAGAIN && err != 0)
		error("failed to read Xresources: %s", strerror(-err));

	if (adopted)
		debug("Xresources available");

	return adopted;
}

void xres_free(void)
{
	if (xres.database)
		xcb_xrm_database_free(xres.database);
	xres.database = NULL;

	if (xres.helper.fd >= 0)
		close(xres.helper.fd);
	xres.helper.fd = -1;

	free(xres.helper.buf);
	xres.helper.buf = NULL;
	xres.helper.len = 0;
}
