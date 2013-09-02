
PROG=showtime-shell

${PROG}: ${PROG}.c
	${CC}  -Wall -o $@ $<

clean:
	rm -f ${PROG}
	rm -f *~
