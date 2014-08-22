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
	struct event	 et_event;
	struct timeval	 et_wait;
};

void	 socket_start(int);
void	 socket_write(int, struct event_time *);
void	 socket_callback(int, short, void *);

struct event_base	*eb;
const char		*host, *port;
int			 family = PF_UNSPEC;
unsigned int		 again_percentage;
unsigned int		 resend_bound = 10, wait_bound = 30;
int			 connected, oneshot, verbose;
struct sockaddr_storage	 lsa, fsa;
socklen_t		 lsalen, fsalen;
int			 socktype, protocol;
char			 laddress[NI_MAXHOST],
			 faddress[NI_MAXHOST], fservice[NI_MAXSERV];

void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-46cosv] [-a again] [-i icmp] [-n num] [-p payload] "
	    "[-r resend] [-w wait] host port\n"
	    "    -4  IPv4 only\n"
	    "    -6  IPv6 only\n"
	    "    -a  percentage of responses that are requested again\n"
	    "    -c  use connected sockets to send packets\n"
	    "    -i  percentage of requests that are icmp errors\n"
	    "    -n  number of simultanously connected sockets (%u)\n"
	    "    -o  oneshot, do not reopen socket\n"
	    "    -p  maximum udp packet payload size\n"
	    "    -r  maximum resend timeout for the query in seconds (%u)\n"
	    "    -s  print statistics every second\n"
	    "    -v  be verbose, print address and service\n"
	    "    -w  maximum wait timeout for the response in seconds (%u)\n",
	    getprogname(), socket_number, resend_bound, wait_bound);
	exit(2);
}

void
setopt(int argc, char *argv[])
{
	const char	*errstr;
	int		 ch;

	while ((ch = getopt(argc, argv, "46a:ci:n:op:r:svw:")) != -1) {
		switch (ch) {
		case '4':
			family = PF_INET;
			break;
		case '6':
			family = PF_INET6;
			break;
		case 'a':
			again_percentage = strtonum(optarg, 0, 100, &errstr);
			if (errstr)
				errx(1, "request again percentage is %s: %s",
				    errstr, optarg);
			break;
		case 'c':
			connected = 1;
			break;
		case 'i':
			icmp_percentage = strtonum(optarg, 0, 100, &errstr);
			if (errstr)
				errx(1, "icmp error percentage is %s: %s",
				    errstr, optarg);
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
		case 'p':
			payload_bound = strtonum(optarg, 1, 65508, &errstr);
			if (errstr)
				errx(1, "payload boundary is %s: %s",
				    errstr, optarg);
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
		case 'v':
			verbose = 1;
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
		err(1, "socket family %d, socktype %d, protocol %d",
		    family, socktype, protocol);
	if (connected) {
		if (connect(s, (struct sockaddr *)&fsa, fsalen) == -1)
			err(1, "connect foreign address %s, service %s",
			    faddress, fservice);
	} else {
		switch (family) {
		case AF_INET:
			((struct sockaddr_in *)&lsa)->sin_port = 0;
			break;
		case AF_INET6:
			((struct sockaddr_in6 *)&lsa)->sin6_port = 0;
			break;
		}
		if (bind(s, (struct sockaddr *)&lsa, lsalen) == -1)
			err(1, "bind local address %s", laddress);
	}
	if ((et = malloc(sizeof(*et))) == NULL)
		err(1, "malloc");
	event_set(&et->et_event, s, EV_READ|EV_PERSIST, socket_callback, et);
	et->et_wait.tv_sec = arc4random_uniform(wait_bound);
	et->et_wait.tv_usec = 1 + arc4random_uniform(999999);
	socket_write(s, et);
	stat_open++;
}

void
socket_write(int s, struct event_time *et)
{
	struct timeval	 to;

	if (family == AF_INET && icmp_percentage &&
	    icmp_percentage > arc4random_uniform(100)) {
		lsalen = sizeof(lsa);
		if (getsockname(s, (struct sockaddr *)&lsa, &lsalen) == -1)
			err(1, "getsockname");
		icmp_send((struct sockaddr_in *)&lsa, lsalen,
		    (struct sockaddr_in *)&fsa, fsalen);
	} else {
		if (connected)
			socket_send(s, "foo\n", NULL, 0);
		else
			socket_send(s, "foo\n",
			    (struct sockaddr *)&fsa, fsalen);
	}

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

		if (again_percentage &&
		    again_percentage > arc4random_uniform(100))
			return;
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
		if (icmp_percentage)
			icmp_destroy();
		statistic_destroy();
	}
}

void
socket_init(void)
{
	struct addrinfo	 hints, *res, *res0;
	const char	*cause = NULL;
	int		 s;
	int		 error, save_errno;
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
	if (verbose)
		printf("%s foreign address %s, service %s\n",
		    getprogname(), faddress, fservice);
	if (res->ai_addrlen > sizeof(fsa))
		err(1, "getaddrinfo: addrlen %u too big", res->ai_addrlen);
	memcpy(&fsa, res->ai_addr, res->ai_addrlen);
	fsalen = res->ai_addrlen;
	family = res->ai_family;
	socktype = res->ai_socktype;
	protocol= res->ai_protocol;
	freeaddrinfo(res0);

	if (!connected) {
		/*
		 * We need multiple bind sockets.  They should be bound to
		 * the same address but use random ports.
		 */
		lsalen = sizeof(lsa);
		if (getsockname(s, (struct sockaddr *)&lsa, &lsalen) == -1)
			err(1, "getsockname");
		error = getnameinfo((struct sockaddr *)&lsa, lsalen,
		    laddress, sizeof(laddress), NULL, 0,
		    NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV);
		if (error)
			errx(1, "getnameinfo local: %s", gai_strerror(error));

		if (verbose)
			printf("%s local address %s\n",
			    getprogname(), laddress);
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
