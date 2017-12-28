VERSION = 1.0

CC ?= gcc
PREFIX ?= /usr/local

PACKAGES = gtk+-3.0
PKG_CFLAGS = `pkg-config --cflags $(PACKAGES)`
PKG_LDFLAGS = `pkg-config --libs $(PACKAGES)`
MYCFLAGS = $(CFLAGS) -std=c99 -Wall
MYLDFLAGS = $(LDFLAGS) -lm

%.o: %.c Makefile
	$(CC) $(PKG_CFLAGS) $(MYCFLAGS) $< -c -o $@

manitor: manitor.o info.o
	$(CC) -o $@ `pkg-config --libs $(PACKAGES)` $^ $(PKG_LDFLAGS) $(MYLDFLAGS)

info.o: info.h
manitor.o: info.h conf.h

clean:
	-rm -f manitor info.o manitor.o

install: manitor
	install -m700 manitor $(DESTDIR)$(PREFIX)/bin/

install-home: manitor
	install -m700 manitor $(HOME)/.local/bin/

.PHONY: clean install install-home
