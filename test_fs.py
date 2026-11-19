import os

mount_path = "mnt"

# 1. Write a file using Python
file_path = os.path.join(mount_path, "from_python.txt")
with open(file_path, "w", encoding="utf-8") as f:
    f.write("Hello from a Python script running on MYFS!\n")
print("[Python] Successfully wrote to file.")

# 2. Read it back
with open(file_path, "r", encoding="utf-8") as f:
    content = f.read()
print(f"[Python] Read back content: {content.strip()}")

# 3. Read the live virtual disk statistics (.diskstat)
stat_path = os.path.join(mount_path, ".diskstat")
with open(stat_path, "r", encoding="utf-8") as f:
    print("\n--- Live Disk Stat via Python ---")
    print(f.read())
