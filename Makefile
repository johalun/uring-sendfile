# Need these flags when we include <fcntl> and <linux/fcntl>
CFLAGS="-DHAVE_ARCH_STRUCT_FLOCK -DHAVE_ARCH_STRUCT_FLOCK64"

all:	server.c
	cc server.c -o server -luring ${CFLAGS}

clean:
	rm server
