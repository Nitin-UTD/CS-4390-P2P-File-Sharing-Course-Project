CC=gcc
CFLAGS=-Wall -Wextra -Werror -O2

all: tracker peer

tracker: tracker.c common.h
	$(CC) $(CFLAGS) -o tracker tracker.c

peer: peer.c common.h
	$(CC) $(CFLAGS) -o peer peer.c

clean:
	rm -f tracker peer *.o *.track
	rm -rf tracker_db

.PHONY: all clean
