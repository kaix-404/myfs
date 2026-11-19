CC = cc
CFLAGS = -Wall $(shell pkg-config fuse3 --cflags)
LDFLAGS = $(shell pkg-config fuse3 --libs)

all: mkfs_myfs myfs

mkfs_myfs: mkfs_myfs.c fs_layout.h
	$(CC) -Wall mkfs_myfs.c -o mkfs_myfs

myfs: myfs.c fs_layout.h
	$(CC) $(CFLAGS) myfs.c $(LDFLAGS) -o myfs

format: mkfs_myfs
	./mkfs_myfs

mount: myfs
	mkdir -p mnt
	./myfs -f -s mnt

unmount:
	fusermount3 -uz mnt || true

clean: unmount
	rm -f mkfs_myfs myfs disk.img
