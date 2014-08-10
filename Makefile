# Fill out these variables as you have to test divert with the pf
# kernel running on a remote machine.  You have to specify a local
# and remote ip address for the test connections.
# You must have an anchor "regress" for the divert rules in the pf.conf
# of the remote machine.  The kernel of the remote machine gets testet.
#
# Run make check-setup to see if you got the setup correct.

LOCAL_ADDR ?=
REMOTE_ADDR ?=
REMOTE_SSH ?=

.if ! empty (REMOTE_SSH)
.if make (regress) || make (all)
.BEGIN:
	@echo
	ssh -t ${REMOTE_SSH} ${SUDO} true
	rm -f stamp-pfctl
.endif
.endif

# compile client and server program for send and receive test packets

SRCS =		client.c server.c util.c
CLEANFILES +=	*.o stamp-* ktrace.out sudpclient sudpserver
CDIAGFLAGS +=	-Wall -Werror \
		-Wbad-function-cast \
		-Wcast-align \
		-Wcast-qual \
		-Wdeclaration-after-statement \
		-Wextra -Wno-unused-parameter \
		-Wmissing-declarations \
		-Wmissing-prototypes \
		-Wshadow -Wpointer-arith \
		-Wsign-compare \
		-Wstrict-prototypes
DEBUG =		-g
LDFLAGS =	-levent
NOMAN =		yes
WARNINGS =	yes

prog: sudpclient sudpserver
sudpclient: client.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} client.o util.o ${LDADD}
sudpserver: server.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} server.o util.o ${LDADD}

# run regression tests, client may run on the remote machine

PORT1 ?=	12345
PORT2 ?=	54321

.if empty (REMOTE_SSH)
SSH =
ONESHOT =	-o
HOST =		localhost
BIND =
STAMP_REMOTE =
.else
SSH =		ssh ${REMOTE_SSH}
ONESHOT =
HOST =		${LOCAL_ADDR}
BIND =		-b ${LOCAL_ADDR}
STAMP_REMOTE =	stamp-scp stamp-pfctl
.endif

CLIENT =	${SSH} ./sudpclient ${ONESHOT} -v -r 3 -w 10 -n 300 -4
SERVER =	${SUDO} ./sudpserver ${ONESHOT} -sv -n 1000 -i 90 -4 ${BIND}

.PHONY: kill

# copy client and server program to remote test machine
kill:
	${SSH} pkill sudpclient || true
	pkill sudpserver || true

# copy client and server program to remote test machine
stamp-scp: sudpclient sudpserver
	scp sudpclient sudpserver ${REMOTE_SSH}:
	date >$@

# load the pf rules into the kernel of the remote machine
stamp-pfctl:
	echo pass out proto udp to ${LOCAL_ADDR} port { ${PORT1} ${PORT2} } \
	    divert-reply | ssh ${REMOTE_SSH} ${SUDO} pfctl -a regress -f -
	@date >$@

REGRESS_TARGETS =	run-regress-client run-regress-server

regress: sudpclient sudpserver kill ${STAMP_REMOTE}
	@echo '\n======== $@ ========'
	cd ${.CURDIR} && ${MAKE} -j 6 \
	    run-regress-server-bind run-regress-server-connect \
	    run-regress-client-bind-bind run-regress-client-bind-connect \
	    run-regress-client-connect-bind run-regress-client-connect-connect

run-regress-client-bind-bind: sudpclient
	${CLIENT} ${HOST} ${PORT1}
run-regress-client-bind-connect: sudpclient
	${CLIENT} ${HOST} ${PORT2}
run-regress-client-connect-bind: sudpclient
	${CLIENT} -c ${HOST} ${PORT1}
run-regress-client-connect-connect: sudpclient
	${CLIENT} -c ${HOST} ${PORT2}
run-regress-server-bind: sudpserver
	${SERVER} ${PORT1}
run-regress-server-connect: sudpserver
	${SERVER} -c ${PORT2}

.PHONY: check-setup

# Check wether the address, route and remote setup is correct
check-setup:
.if ! empty (REMOTE_SSH)
	@echo '\n======== $@ ADDR ========'
	ping -n -c 1 ${LOCAL_ADDR}  # LOCAL_ADDR
	route -n get ${LOCAL_ADDR} | fgrep -q 'interface: lo0'  # LOCAL_ADDR
	ping -n -c 1 ${REMOTE_ADDR}  # REMOTE_ADDR
	@echo '\n======== $@ PF ========'
	ssh ${PF_SSH} ${SUDO} pfctl -sr | grep '^anchor "regress" all$$'
	ssh ${PF_SSH} ${SUDO} pfctl -si | grep '^Status: Enabled '
.endif

.include <bsd.regress.mk>
