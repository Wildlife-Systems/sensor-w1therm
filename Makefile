# Makefile for sensor-w1therm
#
# Part of the WildlifeSystems project.
# https://wildlife.systems

CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE -I/usr/include/ws
LDFLAGS = -pthread -lwildlifesystems

PREFIX ?= /usr
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

TARGET = sensor-w1therm
SRCDIR = src
SOURCES = $(SRCDIR)/sensor-w1therm.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 man/sensor-w1therm.1 $(DESTDIR)$(MANDIR)/sensor-w1therm.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/sensor-w1therm.1

clean:
	rm -f $(TARGET) $(OBJECTS)
