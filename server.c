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
#include <sys/uio.h>

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
	struct sockaddr_storage  ea_lsa, ea_fsa;
	int			 ea_family, ea_socktype, ea_protocol;
	socklen_t                ea_lsalen, ea_fsalen;
};

ssize_t	 socket_recv(int, struct event_addr *);
void	 socket_read(int, struct event_addr *);
void	 socket_write(int, struct event_addr *);
void	 socket_callback(int, short, void *);

struct event_base	*eb;
struct event_addr	*eladdr;
const char		*host, *port;
int			 family = PF_UNSPEC;
unsigned int		 delay_bound = 10;
unsigned int		 icmp_percentage;
int			 connected, oneshot, verbose;
char			 laddress[NI_MAXHOST], lservice[NI_MAXSERV];

void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-46cosv] [-b bind] [-d delay] [-i icmp] [-n num] "
	    " [-p payload] port\n"
	    "    -4  IPv4 only\n"
	    "    -6  IPv6 only\n"
	    "    -b  bind socket to address\n"
	    "    -c  use connected sockets to send packets\n"
	    "    -d  maximum delay for the response in seconds (%u)\n"
	    "    -i  percentage of responses that are icmp errors\n"
	    "    -n  maximum number of simultanously bind sockets (%u)\n"
	    "    -o  oneshot, do not reopen socket\n"
	    "    -p  maximum udp packet payload size\n"
	    "    -s  print statistics every second\n"
	    "    -v  be verbose, print address and service\n",
	    getprogname(), socket_number, delay_bound);
	exit(2);
}

void
setopt(int argc, char *argv[])
{
	const char	*errstr;
	int		 ch;

	while ((ch = getopt(argc, argv, "46b:cd:i:n:op:sv")) != -1) {
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
		case 'd':
			delay_bound = strtonum(optarg, 1, 60, &errstr);
			if (errstr)
				errx(1, "delay boundary time is %s: %s",
				    errstr, optarg);
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
		case 's':
			statistics = 1;
			break;
		case 'v':
			verbose = 1;
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
}

ssize_t
socket_recv(int s, struct event_addr *ea)
{
	char		 rbuf[16];
	struct iovec	 iov;
	struct msghdr	 msg;
	struct cmsghdr	*cmsg;
	union {
		struct cmsghdr	 hdr;
		unsigned char	 buf[CMSG_SPACE(sizeof(struct in_addr))+
				    CMSG_SPACE(sizeof(in_port_t))];
		unsigned char	 buf6[CMSG_SPACE(sizeof(struct in6_pktinfo))+
				    CMSG_SPACE(sizeof(in_port_t))];
	} cmsgbuf;
	ssize_t		 n;

	iov.iov_base = rbuf;
	iov.iov_len = sizeof(rbuf);
	msg.msg_name = &ea->ea_fsa;
	msg.msg_namelen = sizeof(ea->ea_fsa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = &cmsgbuf.buf;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_flags = 0;

	if ((n = recvmsg(s, &msg, 0)) == -1)
		return (n);

	ea->ea_fsalen = ea->ea_fsa.ss_len;
	if (msg.msg_flags & MSG_CTRUNC)
		errx(1, "recvmsg: control message truncated");
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
	    cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_len == CMSG_LEN(sizeof(struct in_addr)) &&
		    cmsg->cmsg_level == IPPROTO_IP &&
		    cmsg->cmsg_type == IP_RECVDSTADDR) {
			((struct sockaddr_in *)&ea->ea_lsa)->sin_addr =
			    *(struct in_addr *)CMSG_DATA(cmsg);
		    ea->ea_lsalen = sizeof(struct sockaddr_in);
		}
		if (cmsg->cmsg_len == CMSG_LEN(sizeof(in_port_t)) &&
		    cmsg->cmsg_level == IPPROTO_IP &&
		    cmsg->cmsg_type == IP_RECVDSTPORT) {
			((struct sockaddr_in *)&ea->ea_lsa)->sin_port =
			    *(in_port_t *)CMSG_DATA(cmsg);
		}
		if (cmsg->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo)) &&
		    cmsg->cmsg_level == IPPROTO_IPV6 &&
		    cmsg->cmsg_type == IPV6_PKTINFO) {
			((struct sockaddr_in6 *)&ea->ea_lsa)->sin6_addr =
			    ((struct in6_pktinfo *)CMSG_DATA(cmsg))->ipi6_addr;
		    ea->ea_lsalen = sizeof(struct sockaddr_in6);
		}
		if (cmsg->cmsg_len == CMSG_LEN(sizeof(in_port_t)) &&
		    cmsg->cmsg_level == IPPROTO_IPV6 &&
		    cmsg->cmsg_type == IPV6_RECVDSTPORT) {
			((struct sockaddr_in6 *)&ea->ea_lsa)->sin6_port =
			    *(in_port_t *)CMSG_DATA(cmsg);
		}
	}

	return (n);
}

void
socket_read(int s, struct event_addr *ea)
{
	struct timeval	 to;
	char		 rbuf[16];

	if (ea->ea_fsalen) {
		/*
		 * The socket is already conntect to the foreign address.
		 * Just read the packet.
		 */
		if (recv(s, rbuf, sizeof(rbuf), 0) == -1) {
			stat_rcverr++;
			if (close(s) == -1)
				err(1, "close");
			event_del(&ea->ea_event);
			free(ea);
			stat_open--;
			return;
		}
	} else {
		struct event_addr	*ef;

		/*
		 * Create an event that is used to send the resonse.  The
		 * local address is the same as we used to bind the socket
		 * where we received the packet.  The foreign address is
		 * taken from the query packet.  The response gets delayed.
		 */
		if ((ef = malloc(sizeof(*ef))) == NULL)
			err(1, "malloc");
		ef->ea_family = ea->ea_family;
		ef->ea_socktype = ea->ea_socktype;
		ef->ea_protocol = ea->ea_protocol;

		if (socket_recv(s, ef) == -1) {
			stat_rcverr++;
			free(ef);
			return;
		}
		if (connected) {
			int	 optval;

			/*
			 * We should use a connected socket, but received
			 * the packet on the unconnected bind socket.  So
			 * we need an additional socket.
			 */
			if ((s = socket(ea->ea_family, ea->ea_socktype,
			    ea->ea_protocol)) == -1) {
				if (errno == EMFILE) {
					stat_error++;
					free(ef);
					return;
				}
				err(1, "socket");
			}
			optval = 1;
			if (setsockopt(s, SOL_SOCKET, SO_REUSEPORT,
			    &optval, sizeof(optval)) == -1)
				err(1, "setsockopt reuseport");
			if (bind(s, (struct sockaddr *)&ea->ea_lsa,
			    ea->ea_lsalen) == -1)
				err(1, "bind");
			if (connect(s, (struct sockaddr *)&ef->ea_fsa,
			    ef->ea_fsalen) == -1) {
				if (errno == EADDRINUSE) {
					stat_error++;
					if (close(s) == -1)
						err(1, "close");
					free(ef);
					return;
				}
				err(1, "connect");
			}
		}
		event_set(&ef->ea_event, s, connected ? EV_READ|EV_TIMEOUT :
		    EV_TIMEOUT, socket_callback, ef);

		ea = ef;
		stat_open++;
	}

	stat_recv++;
	to.tv_sec = arc4random_uniform(delay_bound);
	to.tv_usec = 1 + arc4random_uniform(999999);
	event_add(&ea->ea_event, &to);
}

void
socket_write(int s, struct event_addr *ea)
{
	if (ea->ea_family == AF_INET && icmp_percentage &&
	    icmp_percentage > arc4random_uniform(100)) {
		icmp_send((struct sockaddr_in *)&ea->ea_lsa, ea->ea_lsalen,
		    (struct sockaddr_in *)&ea->ea_fsa, ea->ea_fsalen);
	} else {
		if (connected)
			socket_send(s, "bar\n", NULL, 0);
		else
			socket_send(s, "bar\n",
			    (struct sockaddr *)&ea->ea_fsa, ea->ea_fsalen);
	}
}

void
socket_callback(int s, short event, void *arg)
{
	struct event_addr	*ea = arg;

	if (event & EV_READ) {
		socket_read(s, ea);
	}
	if (event & EV_TIMEOUT) {
		/*
		 * The delay for the response is over.  Send it and
		 * destroy the event structure.
		 */
		socket_write(s, ea);
		if (connected) {
			if (close(s) == -1)
				err(1, "close");
		}
		free(ea);
		stat_open--;
	}
	if (oneshot && stat_open == 0) {
		for (ea = eladdr; ea->ea_lsalen; ea++)
			event_del(&ea->ea_event);
		free(eladdr);
		if (icmp_percentage)
			icmp_destroy();
		statistic_destroy();
	}
}

void
socket_init(void)
{
	struct event_addr	*ea;
	struct addrinfo		 hints, *res, *res0;
	const char		*cause = NULL;
	const struct sockaddr	**lsa;
	socklen_t		*lsalen;
	int			*sfamily, *socktype, *protocol;
	int			*s;
	int			 optval = 1;
	int			 error, save_errno;
	unsigned int		 nsock, n;

	/*
	 * Create sockets and bind them for all suitable addresses.
	 */
	if ((s = calloc(socket_number, sizeof(*s))) == NULL)
		err(1, "calloc");
	if ((lsa = calloc(socket_number, sizeof(*lsa))) == NULL)
		err(1, "calloc");
	if ((lsalen = calloc(socket_number, sizeof(*lsalen))) == NULL)
		err(1, "calloc");
	if ((sfamily = calloc(socket_number, sizeof(*sfamily))) == NULL)
		err(1, "calloc");
	if ((socktype = calloc(socket_number, sizeof(*socktype))) == NULL)
		err(1, "calloc");
	if ((protocol = calloc(socket_number, sizeof(*protocol))) == NULL)
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
	for (res = res0; res && nsock < socket_number; res = res->ai_next) {
		s[nsock] = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (s[nsock] == -1) {
			cause = "socket";
			continue;
		}
		switch (res->ai_family) {
		case AF_INET:
			if (setsockopt(s[nsock], IPPROTO_IP, IP_RECVDSTADDR,
			    &optval, sizeof(optval)) == -1)
				err(1, "setsockopt recvdstaddr");
			if (setsockopt(s[nsock], IPPROTO_IP, IP_RECVDSTPORT,
			    &optval, sizeof(optval)) == -1)
				err(1, "setsockopt recvdstport");
			break;
		case AF_INET6:
			if (setsockopt(s[nsock], IPPROTO_IPV6, IPV6_RECVPKTINFO,
			    &optval, sizeof(optval)) == -1)
				err(1, "setsockopt recvpktinfo6");
			if (setsockopt(s[nsock], IPPROTO_IPV6, IPV6_RECVDSTPORT,
			    &optval, sizeof(optval)) == -1)
				err(1, "setsockopt recvdstport6");
			break;
		}

		error = getnameinfo(res->ai_addr, res->ai_addrlen,
		    laddress, sizeof(laddress), lservice, sizeof(lservice),
		    NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV);
		if (error)
			errx(1, "getnameinfo local: %s", gai_strerror(error));

		if (connected) {
			if (setsockopt(s[nsock], SOL_SOCKET, SO_REUSEPORT,
			    &optval, sizeof(optval)) == -1)
				err(1, "setsockopt reuseport");
		}
		if (bind(s[nsock], res->ai_addr, res->ai_addrlen) == -1) {
			cause = "bind";
			save_errno = errno;
			if (close(s[nsock]) == -1)
				err(1, "close");
			errno = save_errno;
			continue;
		}

		if (verbose)
			printf("%s local address %s, service %s\n",
			    getprogname(), laddress, lservice);
		lsa[nsock] = res->ai_addr;
		lsalen[nsock] = res->ai_addrlen;
		sfamily[nsock] = res->ai_family;
		socktype[nsock] = res->ai_socktype;
		protocol[nsock] = res->ai_protocol;
		nsock++;
	}
	if (nsock == 0)
		err(1, "%s local address %s, service %s",
		    cause, laddress, lservice);

	/*
	 * Create an event structure for every socket that has been bound
	 * to an address.  Wait to receive packets on these sockets.
	 */
	if ((ea = eladdr = calloc(nsock + 1, sizeof(*ea))) == NULL)
		err(1, "calloc");
	for (n = 0; n < nsock; n++, ea++) {
		event_set(&ea->ea_event, s[n], EV_READ|EV_PERSIST,
		    socket_callback, ea);
		if (lsalen[n] > sizeof(ea->ea_lsa))
			err(1, "getaddrinfo: addrlen %u too big", lsalen[n]);
		memcpy(&ea->ea_lsa, lsa[n], lsalen[n]);
		ea->ea_lsalen = lsalen[n];
		ea->ea_family = sfamily[n];
		ea->ea_socktype = socktype[n];
		ea->ea_protocol = protocol[n];
		event_add(&ea->ea_event, NULL);
	}
	free(s);
	free(lsa);
	free(lsalen);
	free(sfamily);
	free(socktype);
	free(protocol);
	freeaddrinfo(res0);
}
