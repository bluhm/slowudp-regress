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

#include <sys/types.h>
#include <sys/time.h>

#include <event.h>
#include <signal.h>
#include <stdio.h>

#include "util.h"

void	 statistic_callback(int, short, void *);

struct event		 evstat;
int			 statistics;
unsigned int		 stat_open, stat_send, stat_snderr,
			 stat_recv, stat_rcverr, stat_error;

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
	static int	 line;

	if (line-- == 0 || (event & EV_SIGNAL)) {
		printf(" %7s %7s %7s %7s %7s %7s\n", "open", "send", "snderr",
		    "recv", "rcverr", "error");
		line = 19;
	}
	printf(" %7d %7d %7d %7d %7d %7d\n", stat_open, stat_send, stat_snderr,
	    stat_recv, stat_rcverr, stat_error);
	if (event & EV_TIMEOUT) {
		struct timeval	 to;

		to.tv_sec = 1;
		to.tv_usec = 0;
		signal_add(evs, &to);
		stat_send = stat_snderr = stat_recv = stat_rcverr =
		    stat_error = 0;
	}
}

void
statistic_destroy(void)
{
	if (statistics)
		statistic_callback(SIGINFO, 0, &evstat);
	event_del(&evstat);
}
