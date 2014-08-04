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
HOST =		localhost
BIND =
STAMP_SCP =
SSH =
.else
HOST =		${LOCAL_ADDR}
BIND =		-b ${LOCAL_ADDR}
STAMP_SCP =	stamp-scp
SSH =		ssh ${REMOTE_SSH}
.endif

.if defined(REGRESS_SKIP_SLOW) && ${REGRESS_SKIP_SLOW} != no
ONESHOT =	-o
.else
ONESHOT ?=
.endif

CLIENT =	${SSH} ./sudpclient ${ONESHOT} -v
SERVER =	./sudpserver ${ONESHOT} -sv ${BIND}

REGRESS_TARGETS =	run-regress-client run-regress-server

regress: sudpclient sudpserver ${STAMP_SCP}
	cd ${.CURDIR} && ${MAKE} -j 6 \
	    run-regress-server-bind run-regress-server-connect \
	    run-regress-client-bind-bind run-regress-client-bind-connect \
	    run-regress-client-connect-bind run-regress-client-connect-connect

stamp-scp: sudpclient sudpserver
	-ssh ${REMOTE_SSH} pkill sudpclient sudpserver
	scp sudpclient sudpserver ${REMOTE_SSH}:
	date >$@

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

.include <bsd.regress.mk>
