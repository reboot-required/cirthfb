CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -pedantic -O2
TARGET   = cirthfbd
SRCS     = main.c metrics.c render.c
OBJS     = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

main.o:    main.c    render.h metrics.h
metrics.o: metrics.c metrics.h
render.o:  render.c  render.h

clean:
	rm -f $(OBJS) $(TARGET)

prefix  ?= /usr/local
bindir  ?= $(prefix)/bin

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(bindir)/$(TARGET)
