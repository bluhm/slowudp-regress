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
	cd ${.CURDIR} && ${MAKE} -j 2 run-regress-client run-regress-server

run-regress-client: sudpclient
	./sudpclient -o localhost 4019
run-regress-server: sudpserver
	./sudpserver -o 4019

sudpclient: client.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} client.o util.o ${LDADD}
sudpserver: server.o util.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} server.o util.o ${LDADD}

.include <bsd.regress.mk>
