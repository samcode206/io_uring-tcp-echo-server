build-io_uring:
	gcc io_uring.c -Wall -pedantic -O3 -o server -L usr/local/lib -luring
build-epoll:
	gcc epoll.c -Wall -pedantic -O3 -o server
