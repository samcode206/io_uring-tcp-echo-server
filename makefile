build:
	gcc server.c -Wall -pedantic -O3 -o server -L usr/local/lib -luring