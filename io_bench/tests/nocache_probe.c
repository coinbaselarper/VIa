/* Page-cache residency after F_NOCACHE preads: cc -arch arm64|x86_64 -o nocache_probe tests/nocache_probe.c; ./nocache_probe FILE (>= 40 MiB; reads at +1 MiB)
   Prints, per pread size, how much of the read range mincore reports resident after one pread with F_NOCACHE. Under Rosetta only the first MiB of a pread stays out of the cache. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static double resident(const char* path, size_t off, size_t len, int evict) {
    int fd = open(path, O_RDONLY);
    size_t pg = (size_t)getpagesize();
    void* m = mmap(0, len, PROT_READ, MAP_SHARED, fd, (off_t)off);
    if (evict) msync(m, len, MS_INVALIDATE);
    char* v = malloc(len / pg);
    mincore(m, len, v);
    size_t n = 0;
    for (size_t i = 0; i < len / pg; i++) n += (v[i] & MINCORE_INCORE) != 0;
    munmap(m, len);
    close(fd);
    free(v);
    return 100.0 * (double)n / (double)(len / pg);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const size_t off = 1 << 20;
    const size_t sizes[] = {(size_t)64 << 10, (size_t)1 << 20, (size_t)2 << 20, (size_t)4 << 20, (size_t)16 << 20, (size_t)32 << 20};
    printf("page size %d\n", getpagesize());
    for (int i = 0; i < 6; i++) {
        resident(argv[1], off, sizes[i], 1);
        void* buf;
        posix_memalign(&buf, 16384, sizes[i]);
        int fd = open(argv[1], O_RDONLY);
        fcntl(fd, F_NOCACHE, 1);
        pread(fd, buf, sizes[i], (off_t)off);
        close(fd);
        free(buf);
        printf("one pread of %zu KiB: %.1f%% resident\n", sizes[i] >> 10, resident(argv[1], off, sizes[i], 0));
    }
    return 0;
}
