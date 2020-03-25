CC=gcc
DEPS = *.h
CFLAGS=-I.
OBJ = main.o sqfs_sblk.o sqfs_decompressor.o sqfs_inode.o sqfs_dir.o

all: sqfs

%.o: %.c $(DEPS)
	$(CC) -Wall -c -o $@ $< $(CFLAGS)

sqfs: $(OBJ)
	$(CC) -Wall -o $@ $^ $(CFLAGS) -lz -lm

clean:
	rm -f *.o sqfs core

.PHONY: all sqfs clean
