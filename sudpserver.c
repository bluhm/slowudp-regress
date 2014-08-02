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
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct event_addr {
	struct event		 ea_event;
	struct sockaddr_storage  ea_faddr;
	const struct sockaddr   *ea_laddr;
	socklen_t                ea_laddrlen, ea_faddrlen;
};

void	 usage(void);
void	 statistic_callback(int, short, void *);
void	 socket_init(void);
void	 socket_callback(int, short, void *);

struct event_base	*eb;
struct event		 evstat;
const char		*host, *port;
unsigned int		 reply_bound = 10;
unsigned int		 socket_number = 1000;
int			 oneshot = 0, statistics = 0;
unsigned int		 stat_open, stat_writes, stat_reads, stat_errors;

int
main(int argc, char *argv[])
{
	struct rlimit	 rlim;
	const char	*errstr;
	int		 ch;

	while ((ch = getopt(argc, argv, "b:n:or:s")) != -1) {
		switch (ch) {
		case 'b':
			host = optarg;
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
			reply_bound = strtonum(optarg, 1, 60, &errstr);
			if (errstr)
				errx(1, "reply boundary time is %s: %s",
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
	socket_init();
	signal_set(&evstat, SIGINFO, statistic_callback, &evstat);
	if (statistics)
		statistic_callback(SIGINFO, EV_TIMEOUT, &evstat);
	else
		signal_add(&evstat, NULL);

	event_dispatch();
	return (0);
}

void
statistic_callback(int sig, short event, void *arg)
{
	struct event	*evs = arg;
	static int	 line;

	if (line-- == 0 || (event & EV_SIGNAL)) {
		printf(" %7s %7s %7s %7s\n", "open", "write", "read", "error");
		line = 19;
	}
	printf(" %7d %7d %7d %7d\n",
	    stat_open, stat_writes, stat_reads, stat_errors);
	if (event & EV_TIMEOUT) {
		struct timeval	 to;

		to.tv_sec = 1;
		to.tv_usec = 0;
		signal_add(evs, &to);
		stat_writes = stat_reads = stat_errors = 0;
	}
}

void
socket_callback(int s, short event, void *arg)
{
	struct event_addr	*ea = arg;

	if (event & EV_READ) {
		struct event_addr	*eaddr;
		char			 rbuf[16];

		if ((eaddr = malloc(sizeof(*eaddr))) == NULL)
			err(1, "malloc");
		eaddr->ea_laddr = ea->ea_laddr;
		eaddr->ea_laddrlen = ea->ea_laddrlen;
		event_set(&eaddr->ea_event, s, EV_TIMEOUT, socket_callback,
		    eaddr);

		if (recvfrom(s, rbuf, sizeof(rbuf), 0, (struct sockaddr *)
		    &eaddr->ea_faddr, &eaddr->ea_faddrlen) == -1) {
			stat_errors++;
			free(eaddr);
		} else {
			struct timeval		 to;

			stat_reads++;
			to.tv_sec = arc4random_uniform(reply_bound);
			to.tv_usec = 1 + arc4random_uniform(999999);
			event_add(&eaddr->ea_event, &to);
			stat_open++;
		}
	}
	if (event & EV_TIMEOUT) {
		const char	 wbuf[] = "foo\n";

		if (sendto(s, wbuf, sizeof(wbuf) - 1, 0, (struct sockaddr *)
		    &ea->ea_faddr, ea->ea_faddrlen) == -1)
			stat_errors++;
		else
			stat_writes++;
		free(ea);
		stat_open--;

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

	if ((s = calloc(socket_number, sizeof(*s))) == NULL)
		err(1, "calloc");
	if ((addr = calloc(socket_number, sizeof(*addr))) == NULL)
		err(1, "calloc");
	if ((addrlen = calloc(socket_number, sizeof(*addrlen))) == NULL)
		err(1, "calloc");

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error)
		errx(1, "getaddrinfo: %s", gai_strerror(error));
	nsock = 0;
	*address = *service = '\0';
	for (res = res0; res && nsock < socket_number; res = res->ai_next) {
		error = getnameinfo(res->ai_addr, res->ai_addrlen, address,
		    sizeof(address), service, sizeof(service),
		    NI_NUMERICHOST | NI_NUMERICSERV);
		if (error)
			errx(1, "getaddrinfo: %s", gai_strerror(error));

		s[nsock] = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (s[nsock] == -1) {
			cause = "socket";
			continue;
		}

		if (bind(s[nsock], res->ai_addr, res->ai_addrlen) == -1) {
			cause = "bind";
			save_errno = errno;
			close(s[nsock]);
			errno = save_errno;
			continue;
		}

		printf("bind to address %s, service %s\n", address, service);
		addr[nsock] = res->ai_addr;
		addrlen[nsock] = res->ai_addrlen;
		nsock++;
	}
	if (nsock == 0)
		err(1, "%s: address %s, service %s", cause, address, service);
        /* don't call freeaddrinfo(res0), addr is still referenced */

	if ((ea = calloc(nsock, sizeof(*ea))) == NULL)
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
	(void)fprintf(stderr, "usage: %s [-bnorsw] port\n"
	    "    -b  bind address\n"
	    "    -n  number of simultanously connected sockets (%u)\n"
	    "    -o  oneshot, do not reopen socket\n"
	    "    -r  maximum reply timeout (%u)\n"
	    "    -s  print statistics every second\n",
	    getprogname(), socket_number, reply_bound);
	exit(2);
}
