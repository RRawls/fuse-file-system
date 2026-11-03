#!/bin/sh
# Build and run the FUSE tests
# Run from anywhere; expects user_fs.c, user_fs.h, block.c, block.h in the directory above tests/.
set -e
cd "$(dirname "$0")/.."
FLAGS="$(pkg-config --cflags fuse 2>/dev/null || echo "-D_FILE_OFFSET_BITS=64 -I${FUSE_INC:-/usr/include/fuse}")"
gcc -g -Wall -fsanitize=address,undefined $FLAGS -I. -UFS_SRC='"user_fs.c"' \
    tests/test_fs.c block.c -o tests/test_fs
# MALLOC_PERTURB_ fills fresh heap memory with junk so uninitialized-buffer bugs show up
MALLOC_PERTURB_=165 ./tests/test_fs
