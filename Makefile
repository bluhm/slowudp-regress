# Fill out these variables as you have to test divert with the pf
# kernel running on a remote machine.  You have to specify a local
# and remote ip address for the test connections.
# You must have an anchor "regress" for the divert rules in the pf.conf
# of the remote machine.  The kernel of the remote machine gets testet.
#
# Run make check-setup to see if you got the setup correct.

LOCAL_ADDR ?=
REMOTE_ADDR ?=
LOCAL_ADDR6 ?=
REMOTE_ADDR6 ?=
REMOTE_SSH ?=

# compile client and server program for send and receive test packets

SRCS =		client.c server.c util.c
CLEANFILES +=	*.o ktrace.out sudpclient sudpserver
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

sudpclient: client.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} client.o util.o ${LDADD}
sudpserver: server.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} server.o util.o ${LDADD}

# run regression tests

REGRESS_TARGETS =	run-regress-client run-regress-server

regress: sudpclient sudpserver
	cd ${.CURDIR} && ${MAKE} -j 6 \
	    run-regress-server-bind run-regress-server-connect \
	    run-regress-client-bind-bind run-regress-client-bind-connect \
	    run-regress-client-connect-bind run-regress-client-connect-connect

run-regress-client-bind-bind: sudpclient
	./sudpclient -osv localhost 4020
run-regress-client-bind-connect: sudpclient
	./sudpclient -osv localhost 4021
run-regress-client-connect-bind: sudpclient
	./sudpclient -cosv localhost 4020
run-regress-client-connect-connect: sudpclient
	./sudpclient -cosv localhost 4021
run-regress-server-bind: sudpserver
	./sudpserver -osv 4020
run-regress-server-connect: sudpserver
	./sudpserver -cosv 4021

.include <bsd.regress.mk>
