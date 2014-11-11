VERSION = 1
HANDINDIR = /labs/proxylab/handin/$(shell whoami)

CC = gcc
CFLAGS = -Wall -g -pedantic
LDFLAGS = -lpthread -pthread

OBJS = proxy.o

all: proxy

proxy: $(OBJS)

proxy.o: proxy.c
	$(CC) $(CFLAGS) -c proxy.c -std=c99

handin:
	cp proxy.c $(HANDINDIR)/$(shell whoami)-$(VERSION)-proxy.c
	chmod 600 $(HANDINDIR)/$(shell whoami)-$(VERSION)-proxy.c


clean:
	rm -f *~ *.o proxy core

