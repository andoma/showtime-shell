#CC = /var/tmp/showtime-autobuild/arm-unknown-linux-gnueabi/bin/arm-linux-gnueabihf-gcc

PROG=showtime-shell

${PROG}: ${PROG}.c
	${CC}  -Wall -o $@ $<

clean:
	rm -f ${PROG}
	rm -f *~
