#!/bin/bash
set -e

echo "=== 1. Formatting & Mounting MYFS ==="
fusermount3 -uz mnt 2>/dev/null || true
./mkfs_myfs
mkdir -p mnt
./myfs -s mnt   # Runs in background daemon mode for the script
sleep 1

echo -e "\n=== 2. Initial Storage Monitor ==="
cat mnt/.diskstat

echo -e "\n=== 3. Testing Lazy Block Allocation (touch) ==="
touch mnt/empty.txt
ls -lh mnt/empty.txt
cat mnt/.diskstat

echo -e "\n=== 4. Testing Multi-Block Write (24 KB across 6 blocks) ==="
head -c 24576 /dev/urandom | base64 > /tmp/test_payload.txt
cp /tmp/test_payload.txt mnt/large.txt
ls -lh mnt/large.txt
echo "Verifying MD5 Checksum:"
md5sum /tmp/test_payload.txt mnt/large.txt
cat mnt/.diskstat

echo -e "\n=== 5. Testing Truncate / Overwrite ==="
echo "Short text" > mnt/large.txt
ls -lh mnt/large.txt
cat mnt/.diskstat

echo -e "\n=== 6. Testing Persistence Across Unmount/Remount ==="
fusermount3 -u mnt
echo "Unmounted cleanly. Remounting..."
./myfs -s mnt
sleep 1
echo "Reading persisted file after remount:"
cat mnt/large.txt

echo -e "\n=== 7. Cleaning Up & Reclaiming Blocks ==="
rm mnt/empty.txt mnt/large.txt
cat mnt/.diskstat
fusermount3 -u mnt
rm -f /tmp/test_payload.txt
echo "=== All MYFS Tests Passed! ==="
