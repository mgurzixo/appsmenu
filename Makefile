SRC=xdg-xmenu.c
BIN=xapps
TESTS=$(wildcard tests/test_*)

CFLAGS= -g
LDFLAGS=${CFLAGS}

PREFIX=/usr/local

all: ${BIN}

${BIN}: ${SRC}
	${CC} ${CFLAGS} -o ${BIN} ${SRC} -linih

xapps.o: xapps.c
	${CC} ${CFLAGS} `pkg-config --cflags gtk+-3.0` -c xapps.c

xdg-xmenu.o: xdg-xmenu.c
	${CC} ${CFLAGS} -c xdg-xmenu.c

xapps: xapps.o xdg-xmenu.o
	${CC} ${LDFLAGS} -o xapps xapps.o xdg-xmenu.o `pkg-config --libs gtk+-3.0`  -linih

install:
	install -D -m 755 ${BIN} ${DESTDIR}${PREFIX}/bin/${BIN}
	install -D -m 644 ${BIN}.1 ${DESTDIR}${PREFIX}/share/man/man1/${BIN}.1
	install -D -m 755 ${BIN} ${DESTDIR}${PREFIX}/bin/xapps


uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/${BIN}
	rm -f ${DESTDIR}${PREFIX}/share/man/man1/${BIN}.1
	rm -f ${DESTDIR}${PREFIX}/bin/xapps

profile:
	${CC} -DDEBUG -Wall -o ${BIN}-prof ${SRC} -linih -g -lprofiler
	CPUPROFILE=/tmp/${BIN}.prof CPUPROFILE_FREQUENCY=1000 ./${BIN}-prof -d > /dev/null
	pprof --pdf ./${BIN}-prof /tmp/${BIN}.prof > prof.pdf
	rm -f ${BIN}-prof

clean:
	rm -f ${BIN} apps.o xdg-xmenu.o

test: ${TESTS}

tests/test_*:
	printf "Testing %-70s" $@
	# use arguments in the */args file if provided
	[ -f $@/args ] && args=$$(cat $@/args) || true
	# modify XDG_DATA_* variables to search only the test directory
	XDG_DATA_DIRS= XDG_DATA_HOME=$@ ./xdg-xmenu -d -i hicolor $$args > $@/output
	diff $@/output $@/menu && echo "\033[32mOK\033[0m" || echo "\033[31mFailed\033[0m"
	rm -f $@/output

.PHONY: install uninstall clean test ${TESTS}
# learn something new everyday: use .SILENT to disable all echos
.SILENT: ${TESTS}
# use .ONESHELL to execute all command in one shell invocation, see $args variable
.ONESHELL: ${TESTS}
# use .NOTPARALLEL to force serial execution
.NOTPARALLEL: test
