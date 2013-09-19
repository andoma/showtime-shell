
PROG=showtime-shell

${PROG}: ${PROG}.c
	${CC}  -Wall -o $@ $< -lpthread

clean:
	rm -f ${PROG}
	rm -f *~
