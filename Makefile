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

sudpclient: client.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} client.o util.o ${LDADD}
sudpserver: server.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} server.o util.o ${LDADD}

.include <bsd.regress.mk>
