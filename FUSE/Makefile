CC=gcc
CFLAGS=-g -Wall -D_FILE_OFFSET_BITS=64
LDFLAGS=-lfuse

OBJ=user_fs.o block.o

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

user_fs: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o user_fs

.PHONY: clean
clean:
	rm -f *.o user_fs

