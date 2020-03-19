CC=gcc
CFLAGS= -I inc

ls: src/ls.c 
	$(CC) -o ls src/ls.c $(CFLAGS)     
	
