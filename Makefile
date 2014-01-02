
PROG=showtime-shell

${PROG}: ${PROG}.c
	${CC} -O2 -Wall -o $@ $< -lpthread

clean:
	rm -f ${PROG}
	rm -f *~
