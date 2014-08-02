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

#include <err.h>
#include <errno.h>
#include <event.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

struct event_time {
	struct event	et_event;
	struct timeval	et_wait;
};

void	 usage(void);
void	 findaddr(void);
void	 socket_init(void);
void	 socket_write(int, struct event_time *);
void	 socket_callback(int, short, void *);

struct event_base	*eb;
const char		*host, *port;
unsigned int		 retry_bound = 10, wait_bound = 30;
unsigned int		 socket_number = 1000;
int			 oneshot = 0;
const struct sockaddr	*addr;
socklen_t		 addrlen;
int			 family, socktype, protocol;
char			 address[NI_MAXHOST], service[NI_MAXSERV];

int
main(int argc, char *argv[])
{
	struct rlimit	 rlim;
	const char	*errstr;
	unsigned int	 n;
	int		 ch;

	while ((ch = getopt(argc, argv, "n:or:sw:")) != -1) {
		switch (ch) {
		case 'n':
			socket_number = strtonum(optarg, 1, 10000, &errstr);
			if (errstr)
				errx(1, "simultaneous socket number is %s: %s",
				    errstr, optarg);
			break;
		case 'o':
			oneshot = 1;
			break;
		case 'r':
			retry_bound = strtonum(optarg, 1, 60, &errstr);
			if (errstr)
				errx(1, "retry boundary time is %s: %s",
				    errstr, optarg);
			break;
		case 's':
			statistics = 1;
			break;
		case 'w':
			wait_bound = strtonum(optarg, 1, 60, &errstr);
			if (errstr)
				errx(1, "wait boundary time is %s: %s",
				    errstr, optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 2)
		usage();
	host = argv[0];
	port = argv[1];
	findaddr();

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
	printf("connect address %s, service %s\n", address, service);
	for (n = 0; n < socket_number; n++)
		socket_init();
	statistic_init();

	event_dispatch();
	return (0);
}

void
socket_init(void)
{
	struct event_time	*et;
	int			 s;

	if ((s = socket(family, socktype, protocol)) == -1)
		err(1, "socket: family %d, socktype %d, protocol %d",
		    family, socktype, protocol);
	if (connect(s, addr, addrlen) == -1)
		err(1, "connect: address %s, service %s", address, service);
	if ((et = malloc(sizeof(*et))) == NULL)
		err(1, "malloc");
	event_set(&et->et_event, s, EV_READ, socket_callback, et);
	et->et_wait.tv_sec = arc4random_uniform(wait_bound);
	et->et_wait.tv_usec = 1 + arc4random_uniform(999999);
	socket_write(s, et);
	stat_open++;
}

void
socket_write(int s, struct event_time *et)
{
	struct timeval	 to;
	const char	 wbuf[] = "foo\n";

	if (send(s, wbuf, sizeof(wbuf) - 1, 0) == -1)
		stat_errors++;
	else
		stat_writes++;

	to.tv_sec = arc4random_uniform(retry_bound);
	to.tv_usec = 1 + arc4random_uniform(999999);
	if (timercmp(&to, &et->et_wait, <)) {
		timersub(&et->et_wait, &to, &et->et_wait);
	} else {
		to = et->et_wait;
		timerclear(&et->et_wait);
	}
	event_add(&et->et_event, &to);
}

void
socket_callback(int s, short event, void *arg)
{
	struct event_time	*et = arg;

	if (event & EV_READ) {
		char	 rbuf[16];

		if (recv(s, rbuf, sizeof(rbuf), 0) == -1)
			stat_errors++;
		else
			stat_reads++;
	}
	if (event & EV_TIMEOUT) {
		if (timerisset(&et->et_wait)) {
			socket_write(s, et);
			return;
		}
	}
	if (close(s) == -1)
		err(1, "close");
	event_del(&et->et_event);
	free(et);
	stat_open--;
	if (!oneshot)
		socket_init();
	if (stat_open == 0)
		statistic_destroy();
}

void
findaddr(void)
{
	struct addrinfo	 hints, *res, *res0;
	int		 error;
	int		 save_errno;
	int		 s;
	const char	*cause = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error)
		errx(1, "getaddrinfo: %s", gai_strerror(error));
	s = -1;
	for (res = res0; res; res = res->ai_next) {
		s = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (s == -1) {
			cause = "socket";
			continue;
		}

		if (connect(s, res->ai_addr, res->ai_addrlen) == -1) {
			cause = "connect";
			save_errno = errno;
			close(s);
			errno = save_errno;
			s = -1;
			continue;
		}

		break;
	}
	if (s == -1)
		err(1, "%s", cause);
	close(s);
	family = res->ai_family;
	socktype = res->ai_socktype;
	protocol= res->ai_protocol;
	addr = res->ai_addr;
	addrlen = res->ai_addrlen;
	/* don't call freeaddrinfo(res0), addr is still referenced */

	error = getnameinfo(addr, addrlen, address, sizeof(address), service,
	    sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
	if (error)
		errx(1, "getaddrinfo: %s", gai_strerror(error));
}

void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-os] [-n num] [-r retry] [-w wait] host port\n"
	    "    -n  number of simultanously connected sockets (%u)\n"
	    "    -o  oneshot, do not reopen socket\n"
	    "    -r  maximum retry query timeout (%u)\n"
	    "    -s  print statistics every second\n"
	    "    -w  maximum wait for reply timeout (%u)\n",
	    getprogname(), socket_number, retry_bound, wait_bound);
	exit(2);
}