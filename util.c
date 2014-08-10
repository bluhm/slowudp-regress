/*
 * Copyright (c) 2014 Alexander Bluhm <bluhm@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>

#include <netinet/in.h>

#include <err.h>
#include <event.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"

void	 droppriv(void);
void	 icmp_callback(int, short, void *);
void	 statistic_callback(int, short, void *);

struct event_base	*eb;
struct event		 evicmp;
struct event		 evstat;
int			 sicmp;
unsigned int		 icmp_percentage;
int			 statistics;
unsigned int		 stat_open, stat_send, stat_snderr,
			 stat_recv, stat_rcverr, stat_error,
			 stat_sndicmp, stat_rcvicmp;

int
main(int argc, char *argv[])
{
	struct rlimit	 rlim;

	setopt(argc, argv);

	if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
		err(1, "getrlimit number of open files");
	if (rlim.rlim_cur < socket_number + 10) {
		rlim.rlim_cur = socket_number + 10;
		if (setrlimit(RLIMIT_NOFILE, &rlim) == -1)
			err(1, "setrlimit number of open files to %llu",
			    rlim.rlim_cur);
	}

	if ((eb = event_init()) == NULL)
		err(1, "event_init");

	/*
	 * Create a raw socket to send and receive icmp error packets.
	 * XXX IPv6 is not implemented.
	 */
	if (icmp_percentage)
		icmp_init();
	if (geteuid() == 0)
		droppriv();

	/*
	 * Create and bind sockets and hook them into the event loop
	 * for all server adresses.
	 */
	socket_init();

	/*
	 * Print statistic information periodically or at siginfo.
	 */
	statistic_init();

	event_dispatch();
	return (0);
}

void
droppriv(void)
{
	const char	*sudoid, *errstr;

	if ((sudoid = getenv("SUDO_GID")) != NULL) {
		gid_t		 gid;

		gid = strtonum(sudoid, 1, UINT_MAX, &errstr);
		if (errstr)
			errx(1, "SUDO_UID is %s: %s", errstr, sudoid);
		if (setgid(gid) == -1)
			err(1, "setgid %u", gid);
	}
	if ((sudoid = getenv("SUDO_UID")) != NULL) {
		uid_t		 uid;

		uid = strtonum(sudoid, 1, UINT_MAX, &errstr);
		if (errstr)
			errx(1, "SUDO_UID is %s: %s", errstr, sudoid);
		if (setuid(uid) == -1)
			err(1, "setuid %u", uid);
	}
}

void
icmp_init(void)
{
	if ((sicmp = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1)
		err(1, "socket icmp");
	event_set(&evicmp, sicmp, EV_READ|EV_PERSIST,
	    icmp_callback, &evicmp);
	event_add(&evicmp, NULL);
}

void
icmp_callback(int s, short event, void *arg)
{
	char     rbuf[1500];

	if (event & EV_READ) {
		if (recv(sicmp, rbuf, sizeof(rbuf), 0) == -1)
			err(1, "recv icmp");
		stat_rcvicmp++;
	}
}

void
icmp_destroy(void)
{
	event_del(&evicmp);
}

void
statistic_init(void)
{
	signal_set(&evstat, SIGINFO, statistic_callback, &evstat);
	if (statistics)
		statistic_callback(SIGINFO, EV_TIMEOUT, &evstat);
	else
		signal_add(&evstat, NULL);
}

void
statistic_callback(int sig, short event, void *arg)
{
	struct event	*evs = arg;
	const char	*fmt;
	static int	 line;

	if (line-- == 0 || (event & EV_SIGNAL)) {
		fmt = icmp_percentage ? " %7s %7s %7s %7s %7s %7s %7s %7s\n" :
		    " %7s %7s %7s %7s %7s %7s\n";
		printf(fmt, "open", "send", "snderr", "recv", "rcverr",
		    "error", "sndicmp", "rcvicmp");
		line = 19;
	}
	fmt = icmp_percentage ? " %7d %7d %7d %7d %7d %7d %7d %7d\n" :
	    " %7d %7d %7d %7d %7d %7d\n";
	printf(fmt, stat_open, stat_send, stat_snderr, stat_recv, stat_rcverr,
	    stat_error, stat_sndicmp, stat_rcvicmp);
	if (event & EV_TIMEOUT) {
		struct timeval	 to;

		to.tv_sec = 1;
		to.tv_usec = 0;
		signal_add(evs, &to);
		stat_send = stat_snderr = stat_recv = stat_rcverr =
		    stat_error = stat_sndicmp = stat_rcvicmp = 0;
	}
}

void
statistic_destroy(void)
{
	if (statistics)
		statistic_callback(SIGINFO, 0, &evstat);
	event_del(&evstat);
}
