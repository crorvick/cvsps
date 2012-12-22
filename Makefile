VERSION=3.1

CC?=gcc
CFLAGS?=-g -O2 -Wall 
CFLAGS+=-I. -DVERSION=\"$(VERSION)\"
prefix?=/usr/local
OBJS=\
	cbtcommon/debug.o\
	cbtcommon/hash.o\
	cbtcommon/text_util.o\
	cbtcommon/sio.o\
	cbtcommon/tcpsocket.o\
	cvsps.o\
	cache.o\
	util.o\
	stats.o\
	cap.o\
	cvs_direct.o\
	list_sort.o

all: cvsps 

cvsps: $(OBJS)
	$(CC) -o cvsps $(OBJS) -lz

check:
	@(cd test >/dev/null; make --quiet)

cppcheck:
	cppcheck --template gcc --enable=all --suppress=unusedStructMember *.[ch]

# Requires asciidoc
cvsps.1: cvsps.asc
	a2x --doctype manpage --format manpage cvsps.asc
cvsps.html: cvsps.asc
	a2x --doctype manpage --format xhtml cvsps.asc

install: cvsps.1
	[ -d $(prefix)/bin ] || mkdir -p $(prefix)/bin
	[ -d $(prefix)/share/man/man1 ] || mkdir -p $(prefix)/share/man/man1
	install cvsps $(prefix)/bin
	install -m 644 cvsps.1 $(prefix)/share/man/man1

clean:
	rm -f cvsps *.o cbtcommon/*.o core cvsps.spec cvsps.1 cvsps.html

cvsps.spec: cvsps.spec.dist
	echo "Version: $(VERSION)" >cvsps.spec

SOURCES = Makefile *.[ch] cbtcommon/*.[ch] merge_utils.sh
DOCS = README COPYING NEWS cvsps.asc TODO
ALL =  $(SOURCES) $(DOCS) control
cvsps-$(VERSION).tar.gz: $(ALL)
	tar --transform='s:^:cvsps-$(VERSION)/:' --show-transformed-names -cvzf cvsps-$(VERSION).tar.gz $(ALL)

dist: cvsps-$(VERSION).tar.gz

release: cvsps-$(VERSION).tar.gz cvsps.html
	shipper -u -m -t; make clean

.PHONY: install clean version dist check
