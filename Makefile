CC = gcc
CFLAGS = -Wall -g -pedantic
LDFLAGS = -lpthread -pthread

OBJS = proxy.o

all: proxy

proxy: $(OBJS)

proxy.o: proxy.c
	$(CC) $(CFLAGS) -c proxy.c -std=c99

clean:
	rm -f *~ *.o proxy core proxy.log

