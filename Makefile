CC=gcc
CFLAGS=-O2 -Wall
LIBFLAGS=-lpthread
RM=rm
RMFLAGS=-rf

all:		pforward

pforward:	pforward.o
		$(CC) $(LIBFLAGS) pforward.o -o pforward

pforward.o:	pforward.c
		$(CC) $(CFLAGS) -c pforward.c

clean:
		$(RM) $(RMFLAGS) *.o pforward pforward.exe *.txt
