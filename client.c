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
void	 socket_init(void);
void	 socket_start(int);
void	 socket_write(int, struct event_time *);
void	 socket_callback(int, short, void *);

struct event_base	*eb;
const char		*host, *port;
int			 family = PF_UNSPEC;
unsigned int		 resend_bound = 10, wait_bound = 30;
unsigned int		 socket_number = 1000;
int			 connected, oneshot;
struct sockaddr_storage	 laddr;
const struct sockaddr	*faddr;
socklen_t		 laddrlen, faddrlen;
int			 socktype, protocol;
char			 laddress[NI_MAXHOST], lservice[NI_MAXSERV],
			 faddress[NI_MAXHOST], fservice[NI_MAXSERV];

int
main(int argc, char *argv[])
{
	struct rlimit	 rlim;
	const char	*errstr;
	int		 ch;

	while ((ch = getopt(argc, argv, "46cn:or:sw:")) != -1) {
		switch (ch) {
		case '4':
			family = PF_INET;
			break;
		case '6':
			family = PF_INET6;
			break;
		case 'c':
			connected = 1;
			break;
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
			resend_bound = strtonum(optarg, 1, 60, &errstr);
			if (errstr)
				errx(1, "resend boundary time is %s: %s",
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
	 * Find connection address, create and bind socket.
	 * Send on connection sockets and start timeout.
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
socket_start(int s)
{
	struct event_time	*et;

	/*
	 * Create and bind a socket, send a packet and wait for the
	 * response.  Also add a retransmit and wait timeout.
	 */
	if ((s = socket(family, socktype, protocol)) == -1)
		err(1, "socket: family %d, socktype %d, protocol %d",
		    family, socktype, protocol);
	if (connected) {
		if (connect(s, faddr, faddrlen) == -1)
			err(1, "connect foreign address %s, service %s",
			    faddress, fservice);
	} else {
		if (bind(s, (struct sockaddr *)&laddr, laddrlen) == -1)
			err(1, "bind local address %s, service %s",
			    laddress, lservice);
	}
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
	ssize_t		 n;

	if (connected)
		n = send(s, wbuf, sizeof(wbuf) - 1, 0);
	else
		n = sendto(s, wbuf, sizeof(wbuf) - 1, 0, faddr, faddrlen);
	if (n == -1)
		stat_snderr++;
	else
		stat_send++;

	/*
	 * Chose a random resend timeout.  If it is greater than the wait
	 * timeout stop retransmitting.  The wait fields indicates how long
	 * we will have to wait after the next timeout.
	 */
	to.tv_sec = arc4random_uniform(resend_bound);
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
			stat_rcverr++;
		else
			stat_recv++;
	}
	if (event & EV_TIMEOUT) {
		/*
		 * If we have not reached the final wait time,
		 * send another packet and wait for the response.
		 */
		if (timerisset(&et->et_wait)) {
			socket_write(s, et);
			return;
		}
	}
	/*
	 * We close the connection after we got a response or reached the
	 * wait interval.
	 */
	if (close(s) == -1)
		err(1, "close");
	event_del(&et->et_event);
	free(et);
	stat_open--;
	if (!oneshot)
		socket_start(s);
	if (oneshot && stat_open == 0) {
		statistic_destroy();
	}
}

void
socket_init(void)
{
	struct addrinfo	 hints, *res, *res0;
	const char	*cause = NULL;
	int		 error, save_errno, s;
	unsigned int	 n;

	/*
	 * Find a suitable connect address and remember it.  Create
	 * a socket for non-connected send.
	 */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error)
		errx(1, "getaddrinfo host %s, port %s: %s",
		    host, port, gai_strerror(error));
	s = -1;
	for (res = res0; res; res = res->ai_next) {
		s = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (s == -1) {
			cause = "socket";
			continue;
		}

		error = getnameinfo(res->ai_addr, res->ai_addrlen,
		    faddress, sizeof(faddress), fservice, sizeof(fservice),
		    NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV);
		if (error)
			errx(1, "getnameinfo foreign: %s", gai_strerror(error));

		if (connect(s, res->ai_addr, res->ai_addrlen) == -1) {
			cause = "connect";
			save_errno = errno;
			if (close(s) == -1)
				err(1, "close");
			errno = save_errno;
			s = -1;
			continue;
		}

		break;
	}
	if (s == -1)
		err(1, "%s foreign address %s, service %s",
		    cause, faddress, fservice);
	printf("%s foreign address %s, service %s\n",
	    getprogname(), faddress, fservice);
	faddr = res->ai_addr;
	faddrlen = res->ai_addrlen;
	family = res->ai_family;
	socktype = res->ai_socktype;
	protocol= res->ai_protocol;
	/* don't call freeaddrinfo(res0), addr is still referenced */

	if (!connected) {
		/*
		 * We need multiple bind sockets.  They should be bound to
		 * the same address but use random ports.
		 */
		laddrlen = sizeof(laddr);
		if (getsockname(s, (struct sockaddr *)&laddr, &laddrlen) == -1)
			err(1, "getsockname");
		switch (family) {
		case AF_INET:
			((struct sockaddr_in *)&laddr)->sin_port = 0;
			break;
		case AF_INET6:
			((struct sockaddr_in6 *)&laddr)->sin6_port = 0;
			break;
		}
		error = getnameinfo((struct sockaddr *)&laddr, laddrlen,
		    laddress, sizeof(laddress), lservice, sizeof(lservice),
		    NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV);
		if (error)
			errx(1, "getnameinfo local: %s", gai_strerror(error));

		printf("%s local address %s, service %s\n",
		    getprogname(), laddress, lservice);
	}
	if (close(s) == -1)
		err(1, "close");

	/*
	 * Create and connect all sockets and hook them into the event
	 * loop.  The kernel automatically binds the local address.
	 */
	for (n = 0; n < socket_number; n++)
		socket_start(s);
}

void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-46cos] [-n num] [-r resend] [-w wait] host port\n"
	    "    -4  IPv4 only\n"
	    "    -6  IPv6 only\n"
	    "    -c  use connected sockets to send packets\n"
	    "    -n  number of simultanously connected sockets (%u)\n"
	    "    -o  oneshot, do not reopen socket\n"
	    "    -r  maximum resend timeout for the query in seconds (%u)\n"
	    "    -s  print statistics every second\n"
	    "    -w  maximum wait timeout for the response in seconds (%u)\n",
	    getprogname(), socket_number, resend_bound, wait_bound);
	exit(2);
}
