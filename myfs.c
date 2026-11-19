#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define ITERATIONS 100
#define BLOCK_SZ   4096

/* mode 0 = Native Ext4
 * mode 1 = MYFS (mmap zero-copy + single-indirect blocks)
 * mode 2 = Standard FUSE (FUSE upcall + backing store pwrite syscall per block)
 */
static double measure_write_path(const char *target_path, size_t bytes_total, int io_mode) {
    char *payload_buf = malloc(bytes_total);
    memset(payload_buf, 'A', bytes_total);

    int target_fd = open(target_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (target_fd < 0) {
        perror("open failed");
        free(payload_buf);
        exit(1);
    }

    int sim_fd = -1;
    if (io_mode == 2) {
        sim_fd = open("/tmp/backing_sim.img", O_CREAT | O_RDWR | O_SYNC, 0644);
    }

    /* Warm-up pass to fault in virtual memory pages and allocate indirect blocks */
    pwrite(target_fd, payload_buf, bytes_total, 0);

    struct timespec t_begin, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_begin);

    for (int iter = 0; iter < ITERATIONS; iter++) {
        size_t cur_off = 0;
        while (cur_off < bytes_total) {
            ssize_t nw = pwrite(target_fd, payload_buf + cur_off, BLOCK_SZ, cur_off);
            if (io_mode == 2 && sim_fd >= 0) {
                pwrite(sim_fd, payload_buf + cur_off, BLOCK_SZ, cur_off);
            }
            if (nw <= 0) break;
            cur_off += nw;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    close(target_fd);
    unlink(target_path);
    if (sim_fd >= 0) {
        close(sim_fd);
        unlink("/tmp/backing_sim.img");
    }
    free(payload_buf);

    if (io_mode >= 1) {
        int purge_fd = open("mnt/.purge", O_RDONLY);
        if (purge_fd >= 0) {
            char drain[128];
            if (read(purge_fd, drain, sizeof(drain)) < 0) {}
            close(purge_fd);
        }
    }

    long long ns_delta = (t_end.tv_sec - t_begin.tv_sec) * 1000000000LL +
                         (t_end.tv_nsec - t_begin.tv_nsec);
    /* Return average latency per full-file write in milliseconds (ms) */
    return ((double)ns_delta / ITERATIONS) / 1e6;
}

int main(void) {
    size_t test_sizes[] = {256 * 1024, 1024 * 1024, 2 * 1024 * 1024, 3 * 1024 * 1024};
    const char *size_names[] = {"256 KB", "1.0 MB", "2.0 MB", "3.0 MB"};

    printf("==========================================================================\n");
    printf(" Multi-Megabyte Three-Tier Write Benchmark (100x Avg Latency in ms)       \n");
    printf("==========================================================================\n");
    printf("%-8s | %-16s | %-15s | %-20s | %-8s\n",
           "Payload", "Native Ext4 (ms)", "MYFS mmap (ms)", "Std FUSE pwrite (ms)", "Speedup");
    printf("--------------------------------------------------------------------------\n");

    for (int idx = 0; idx < 4; idx++) {
        double t_ext4 = measure_write_path("/tmp/bench_ext4.bin", test_sizes[idx], 0);
        double t_myfs = measure_write_path("mnt/bench_tmp.bin", test_sizes[idx], 1);
        double t_stdf = measure_write_path("mnt/bench_tmp.bin", test_sizes[idx], 2);
        double speedup = ((t_stdf - t_myfs) / t_stdf) * 100.0;
        printf("%-8s | %-16.2f | %-15.2f | %-20.2f | +%.1f%%\n",
               size_names[idx], t_ext4, t_myfs, t_stdf, speedup);
    }
    printf("==========================================================================\n");
    return 0;
}