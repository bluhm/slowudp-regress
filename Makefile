SRCS =		client.c server.c
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

all: sudpclient sudpserver

sudpclient: client.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} client.o ${LDADD}
sudpserver: server.o
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} server.o ${LDADD}

.include <bsd.prog.mk>
