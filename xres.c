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
 * startup delay, every resource is considered unset. The helper writes its
 * result to a pipe as a type byte followed by the resources, if any, and a
 * null byte.
 */

/* Time given to the X server to provide its resources, from startup */
#define XRES_STARTUP_MSEC	1000

/* Time after which the helper gives up on a silent server */
#define XRES_ATTEMPT_SEC	10

#define XRES_NONE	'N'	/* no usable display */
#define XRES_STORE	'S'	/* the RESOURCE_MANAGER property of the server */
#define XRES_FILES	'F'	/* the resource files, the server holds none */

static struct {
	struct timespec deadline;
	bool queried;
	int fd;
	char *buf;
	size_t len;
	xcb_xrm_database_t *database;
} xres = {
	.fd = -1,
};

static long xres_msec_left(const struct timespec *ts)
{
	struct timespec now;
	long msec;

	clock_gettime(CLOCK_MONOTONIC, &now);

	msec = (ts->tv_sec - now.tv_sec) * 1000 +
	       (ts->tv_nsec - now.tv_nsec + 999999) / 1000000;

	return msec > 0 ? msec : 0;
}

int xres_init(void)
{
	if (clock_gettime(CLOCK_MONOTONIC, &xres.deadline))
		return -errno;

	xres.deadline.tv_sec += XRES_STARTUP_MSEC / 1000;
	xres.deadline.tv_nsec += (XRES_STARTUP_MSEC % 1000) * 1000000;
	if (xres.deadline.tv_nsec >= 1000000000) {
		xres.deadline.tv_sec++;
		xres.deadline.tv_nsec -= 1000000000;
	}

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

/* Fetch the resources and write them to fd, in a child process */
static void xres_attempt(int fd)
{
	xcb_xrm_database_t *database = NULL;
	xcb_connection_t *conn;
	xcb_screen_t *screen;
	char type = XRES_NONE;
	char *str = NULL;

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

	if (xres_write(fd, &type, 1) == 0 && str)
		xres_write(fd, str, strlen(str));
	xres_write(fd, "", 1);

	free(str);
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

	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL);
}

static void xres_helper(int fd)
{
	xres_helper_init();
	signal(SIGALRM, SIG_DFL);

	/* Give up on a silent server */
	alarm(XRES_ATTEMPT_SEC);

	xres_attempt(fd);
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

		pid = fork();
		if (pid == 0)
			xres_helper(fds[1]);

		_exit(0);
	}

	close(fds[1]);

	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		continue;

	xres.fd = fds[0];

	return 0;
err:
	close(fds[0]);
	close(fds[1]);

	return -errno;
}

/* Use resources from the helper, a message without them means none */
static void xres_use(char type, const char *str)
{
	xcb_xrm_database_t *database;

	if (type != XRES_STORE && type != XRES_FILES)
		return;

	database = xcb_xrm_database_from_string(str);
	if (!database) {
		error("failed to parse the Xresources");
		return;
	}

	if (xres.database)
		xcb_xrm_database_free(xres.database);

	xres.database = database;
}

/* Read from the helper; return 1 after a message, 0 at the end, or an error */
static int xres_read(void)
{
	char buf[4096];
	char *end, *tmp;
	ssize_t n;
	size_t len;

	for (;;) {
		/* A complete message? */
		end = xres.buf ? memchr(xres.buf, '\0', xres.len) : NULL;
		if (end) {
			xres_use(xres.buf[0], xres.buf + 1);

			len = xres.len - (end + 1 - xres.buf);
			memmove(xres.buf, end + 1, len);
			xres.len = len;

			return 1;
		}

		n = read(xres.fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		if (n == 0)
			return 0;

		tmp = realloc(xres.buf, xres.len + n);
		if (!tmp)
			return -ENOMEM;

		memcpy(tmp + xres.len, buf, n);
		xres.buf = tmp;
		xres.len += n;
	}
}

/* Wait for the result of the helper, until the end of the startup delay */
static void xres_query(void)
{
	struct pollfd pfd;
	const char *display = getenv("DISPLAY");
	int err;

	xres.queried = true;

	/* No display can show up later on either */
	if (!display || *display == '\0')
		return;

	err = xres_spawn();
	if (err) {
		error("failed to query Xresources: %s", strerror(-err));
		return;
	}

	pfd.fd = xres.fd;
	pfd.events = POLLIN;

	for (;;) {
		err = poll(&pfd, 1, xres_msec_left(&xres.deadline));
		if (err < 0 && errno == EINTR)
			continue;

		if (err <= 0) {
			trace("Xresources unavailable within the startup delay");
			break;
		}

		/* Until a complete message, or the end */
		if (xres_read() != -EAGAIN)
			break;
	}

	close(xres.fd);
	xres.fd = -1;
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

void xres_free(void)
{
	if (xres.database)
		xcb_xrm_database_free(xres.database);
	xres.database = NULL;

	if (xres.fd >= 0)
		close(xres.fd);
	xres.fd = -1;

	free(xres.buf);
	xres.buf = NULL;
	xres.len = 0;
}
