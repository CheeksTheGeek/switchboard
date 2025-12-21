#ifndef BARRIER_SYNC_H__
#define BARRIER_SYNC_H__

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#ifdef __cplusplus
#include <atomic>
using namespace std;
#else
#include <stdatomic.h>
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#define BARRIER_CACHE_LINE_SIZE 64

// all procs share this structure via mmap
typedef struct cycle_barrier_shared {
    // the current global cycle count - (is incremented by leader after all processes sync)
    uint64_t cycle_count __attribute__((__aligned__(BARRIER_CACHE_LINE_SIZE)));
    // num of procs that have arrived at the current barrier
    uint32_t barrier_count __attribute__((__aligned__(BARRIER_CACHE_LINE_SIZE)));
    // num of processes participating in synchronization
    uint32_t num_processes __attribute__((__aligned__(BARRIER_CACHE_LINE_SIZE)));

    // Sense flag - alternates 0/1 each barrier (sense-reversing barrier)
    uint32_t sense __attribute__((__aligned__(BARRIER_CACHE_LINE_SIZE)));

    // Flag indicating barrier is initialized and ready
    uint32_t initialized __attribute__((__aligned__(BARRIER_CACHE_LINE_SIZE)));
} cycle_barrier_shared;

typedef struct cycle_barrier {
    cycle_barrier_shared* shm;
    char* name;
    int fd;
    bool is_leader;
    uint32_t local_sense;  // Each process tracks its expected sense value
    bool unmap_at_close;
} cycle_barrier;

// Calculate required map size for barrier shared memory
static inline size_t barrier_mapsize(void) {
    return sizeof(cycle_barrier_shared);
}

// Initialize barrier structure (called by leader process only)
static inline void barrier_init_shared(cycle_barrier_shared* shm, uint32_t num_processes) {
    __atomic_store_n(&shm->cycle_count, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shm->barrier_count, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shm->num_processes, num_processes, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shm->sense, 0, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&shm->initialized, 1, __ATOMIC_SEQ_CST);
}

// open/create barrier for synchronization
// is_leader=true -> creates and initializes the barrier
// is_leader=false -> waits for leader to initialize
static inline cycle_barrier* barrier_open(const char* name, bool is_leader, uint32_t num_processes) {
    cycle_barrier* b = NULL;
    size_t mapsize;
    void* p;
    int fd = -1;
    int r;
    int flags;
    int retries = 0;
    const int max_retries = 1000;  // max wait ~10s

    mapsize = barrier_mapsize();

    // alloc aligned barrier structure
    r = posix_memalign(&p, BARRIER_CACHE_LINE_SIZE, sizeof(cycle_barrier));
    if (r) {
        fprintf(stderr, "barrier_open: posix_memalign failed: %s\n", strerror(r));
        goto err;
    }
    b = (cycle_barrier*)p;
    memset(b, 0, sizeof(*b));

    // open/create shmem file
    flags = O_RDWR;
    if (is_leader) {
        flags |= O_CREAT | O_TRUNC;
    }
    retries = 0;
    while (retries < max_retries) {
        fd = open(name, flags, S_IRUSR | S_IWUSR);
        if (fd >= 0) break;

        if (!is_leader && errno == ENOENT) {
            // follower waiting for leader to create file
            usleep(10000);  // expect 10ms
            retries++;
            continue;
        }

        perror(name);
        goto err;
    }

    if (fd < 0) {
        fprintf(stderr, "barrier_open: timeout waiting for barrier file %s\n", name);
        goto err;
    }

    // file size (leader only)
    if (is_leader) {
        r = ftruncate(fd, mapsize);
        if (r < 0) {
            perror("barrier_open: ftruncate");
            goto err;
        }
    } else {
        // follower waits for file to be properly sized
        retries = 0;
        while (retries < max_retries) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size >= (off_t)mapsize) {
                break;
            }
            usleep(10000);
            retries++;
        }
        if (retries >= max_retries) {
            fprintf(stderr, "barrier_open: timeout waiting for barrier file to be sized\n");
            goto err;
        }
    }

    // map shmem
    p = mmap(NULL, mapsize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (p == MAP_FAILED) {
        perror("barrier_open: mmap");
        goto err;
    }

    b->shm = (cycle_barrier_shared*)p;
    b->name = strdup(name);
    b->fd = fd;
    b->is_leader = is_leader;
    b->local_sense = 1;  // start expecting sense=1 (will be set by first barrier)
    b->unmap_at_close = true;

    // leader inits shared structure
    if (is_leader) {
        barrier_init_shared(b->shm, num_processes);
    } else {
        // all "follower" processes wait for initialization
        retries = 0;
        while (retries < max_retries) {
            if (__atomic_load_n(&b->shm->initialized, __ATOMIC_ACQUIRE) == 1) {
                break;
            }
            usleep(1000);
            retries++;
        }
        if (retries >= max_retries) {
            fprintf(stderr, "barrier_open: timeout waiting for barrier initialization\n");
            goto err;
        }
    }

    return b;

err:
    if (fd >= 0) {
        close(fd);
    }
    free(b);
    return NULL;
}

// close barrier and free resources
static inline void barrier_close(cycle_barrier* b) {
    if (!b) return;

    if (b->unmap_at_close && b->shm) {
        munmap(b->shm, barrier_mapsize());
    }

    if (b->fd >= 0) {
        close(b->fd);
    }

    // Leader removes the file
    if (b->is_leader && b->name) {
        unlink(b->name);
    }

    free(b->name);
    free(b);
}

// waiting @ barrier - all processes must call this each cycle
// returns: the current synchronized cycle count
// implements a sense-reversing barrier algorithm
// SEQ_CST ordering for cross-process safety via shared memory
static inline uint64_t barrier_wait(cycle_barrier* b) {
    assert(b && b->shm);

    cycle_barrier_shared* shm = b->shm;
    uint32_t num_procs = __atomic_load_n(&shm->num_processes, __ATOMIC_SEQ_CST);
    uint32_t my_sense = b->local_sense;  // The sense value we're waiting for

    // Increment barrier count atomically
    uint32_t arrived = __atomic_add_fetch(&shm->barrier_count, 1, __ATOMIC_SEQ_CST);

    if (arrived == num_procs) {
        // Last process to arrive - release barrier

        // Reset barrier count for next barrier FIRST
        __atomic_store_n(&shm->barrier_count, 0, __ATOMIC_SEQ_CST);

        // Full memory barrier
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Increment cycle count
        __atomic_add_fetch(&shm->cycle_count, 1, __ATOMIC_SEQ_CST);

        // Set sense to release waiting processes
        // This is the signal that all have arrived
        __atomic_store_n(&shm->sense, my_sense, __ATOMIC_SEQ_CST);
    } else {
        // Wait for sense to match expected value
        while (__atomic_load_n(&shm->sense, __ATOMIC_SEQ_CST) != my_sense) {
            // Busy wait with pause hint for better CPU efficiency
            #if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__("pause");
            #elif defined(__aarch64__)
            __asm__ __volatile__("yield");
            #endif
        }
    }

    // full fence before proceeding to ensure we see all updates from the releaser
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // flip local sense for next barrier (sense-reversing)
    b->local_sense = 1 - my_sense;

    // return current cycle count
    return __atomic_load_n(&shm->cycle_count, __ATOMIC_SEQ_CST);
}

// get current cycle count without waiting
static inline uint64_t barrier_get_cycle(cycle_barrier* b) {
    assert(b && b->shm);
    return __atomic_load_n(&b->shm->cycle_count, __ATOMIC_ACQUIRE);
}

// check if all processes are ready (registered)
static inline bool barrier_all_ready(cycle_barrier* b) {
    assert(b && b->shm);
    return __atomic_load_n(&b->shm->initialized, __ATOMIC_ACQUIRE) == 1;
}

// update num procs (for dynamic RM loading)
// only leader should call this, and only when all processes are at barrier
static inline void barrier_set_num_processes(cycle_barrier* b, uint32_t num_processes) {
    assert(b && b->shm && b->is_leader);
    __atomic_store_n(&b->shm->num_processes, num_processes, __ATOMIC_RELEASE);
}

// get numb procs
static inline uint32_t barrier_get_num_processes(cycle_barrier* b) {
    assert(b && b->shm);
    return __atomic_load_n(&b->shm->num_processes, __ATOMIC_ACQUIRE);
}

#endif // BARRIER_SYNC_H__
