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

struct event_addr {
	struct event		 ea_event;
	struct sockaddr_storage  ea_faddr;
	const struct sockaddr   *ea_laddr;
	socklen_t                ea_laddrlen, ea_faddrlen;
};

void	 usage(void);
void	 socket_init(void);
void	 socket_read(int, struct event_addr *);
void	 socket_callback(int, short, void *);

struct event_base	*eb;
struct event_addr	*eladdr;
int			 family = PF_UNSPEC;
const char		*host, *port;
unsigned int		 delay_bound = 10;
unsigned int		 socket_number = 1000;
int			 connected, oneshot;

int
main(int argc, char *argv[])
{
	struct rlimit	 rlim;
	const char	*errstr;
	int		 ch;

	while ((ch = getopt(argc, argv, "46bc:n:or:s")) != -1) {
		switch (ch) {
		case '4':
			family = PF_INET;
			break;
		case '6':
			family = PF_INET6;
			break;
		case 'b':
			host = optarg;
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
			delay_bound = strtonum(optarg, 1, 60, &errstr);
			if (errstr)
				errx(1, "delay boundary time is %s: %s",
				    errstr, optarg);
			break;
		case 's':
			statistics = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 1)
		usage();
	port = argv[0];

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
socket_read(int s, struct event_addr *ea)
{
	struct event_addr	*ef;
	struct timeval		 to;
	char			 rbuf[16];

	/*
	 * Create an event that is used to send the resonse.  The
	 * local address is the same as we used to bind the socket
	 * where we received the packet.  The foreign address is
	 * taken from the query packet.  The response gets delayed.
	 */
	if ((ef = malloc(sizeof(*ef))) == NULL)
		err(1, "malloc");
	ef->ea_laddr = ea->ea_laddr;
	ef->ea_laddrlen = ea->ea_laddrlen;
	event_set(&ef->ea_event, s, EV_TIMEOUT, socket_callback, ef);

	if (recvfrom(s, rbuf, sizeof(rbuf), 0, (struct sockaddr *)
	    &ef->ea_faddr, &ef->ea_faddrlen) == -1) {
		stat_rcverr++;
		free(ef);
		return;
	}

	stat_recv++;
	to.tv_sec = arc4random_uniform(delay_bound);
	to.tv_usec = 1 + arc4random_uniform(999999);
	event_add(&ef->ea_event, &to);
	stat_open++;
}

void
socket_callback(int s, short event, void *arg)
{
	struct event_addr	*ea = arg;

	if (event & EV_READ) {
		socket_read(s, ea);
	}
	if (event & EV_TIMEOUT) {
		const char	 wbuf[] = "bar\n";

		/*
		 * The delay for the response is over.  Send it and
		 * destroy the event structure.
		 */
		if (sendto(s, wbuf, sizeof(wbuf) - 1, 0, (struct sockaddr *)
		    &ea->ea_faddr, ea->ea_faddrlen) == -1)
			stat_snderr++;
		else
			stat_send++;
		free(ea);
		stat_open--;

	}
	if (oneshot && stat_open == 0) {
		for (ea = eladdr; ea->ea_laddr; ea++)
			event_del(&ea->ea_event);
		free(eladdr);
		statistic_destroy();
	}
}

void
socket_init(void)
{
	struct event_addr	*ea;
	struct addrinfo		 hints, *res, *res0;
	int			 error;
	int			 save_errno;
	int			*s;
	unsigned int		 nsock, n;
	const char		*cause = NULL;
	const struct sockaddr	**addr;
	socklen_t		*addrlen;
	char			 address[NI_MAXHOST], service[NI_MAXSERV];

	/*
	 * Create sockets and bind them for all suitable addresses.
	 */
	if ((s = calloc(socket_number, sizeof(*s))) == NULL)
		err(1, "calloc");
	if ((addr = calloc(socket_number, sizeof(*addr))) == NULL)
		err(1, "calloc");
	if ((addrlen = calloc(socket_number, sizeof(*addrlen))) == NULL)
		err(1, "calloc");

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error)
		errx(1, "getaddrinfo host %s, port %s: %s",
		    host, port, gai_strerror(error));
	nsock = 0;
	*address = *service = '\0';
	for (res = res0; res && nsock < socket_number; res = res->ai_next) {
		error = getnameinfo(res->ai_addr, res->ai_addrlen, address,
		    sizeof(address), service, sizeof(service),
		    NI_NUMERICHOST | NI_NUMERICSERV);
		if (error)
			errx(1, "getnameinfo: %s", gai_strerror(error));

		s[nsock] = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (s[nsock] == -1) {
			cause = "socket";
			continue;
		}

		if (bind(s[nsock], res->ai_addr, res->ai_addrlen) == -1) {
			cause = "bind";
			save_errno = errno;
			if (close(s[nsock]) == -1)
				err(1, "close");
			errno = save_errno;
			continue;
		}

		printf("bind address %s, service %s\n", address, service);
		addr[nsock] = res->ai_addr;
		addrlen[nsock] = res->ai_addrlen;
		nsock++;
	}
	if (nsock == 0)
		err(1, "%s: address %s, service %s", cause, address, service);
	/* don't call freeaddrinfo(res0), addr is still referenced */

	/*
	 * Create an event structure for every socket that has been bound
	 * to an address.  Wait to receive packets on these sockets.
	 */
	if ((ea = eladdr = calloc(nsock + 1, sizeof(*ea))) == NULL)
		err(1, "calloc");
	for (n = 0; n < nsock; n++, ea++) {
		event_set(&ea->ea_event, s[n], EV_READ|EV_PERSIST,
		    socket_callback, &ea);
		ea->ea_laddr = addr[n];
		ea->ea_laddrlen = addrlen[n];
		event_add(&ea->ea_event, NULL);
	}
	free(addr);
	free(addrlen);
	free(s);
}

void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-46cos] [-b bind] [-d delay] [-n num] port\n"
	    "    -4  IPv4 only\n"
	    "    -6  IPv6 only\n"
	    "    -b  bind socket to address\n"
	    "    -c  use connected sockets to send packets\n"
	    "    -d  maximum delay for the response in seconds (%u)\n"
	    "    -n  maximum number of simultanously bind sockets (%u)\n"
	    "    -o  oneshot, do not reopen socket\n"
	    "    -s  print statistics every second\n",
	    getprogname(), socket_number, delay_bound);
	exit(2);
}
