#include <assert.h>
#include <memory>
#include <cstring>

#include "svdpi.h"
#include "../cpp/barrier_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

// init verilog barrier
// uri: path to shared memory file
// is_leader: 1 if this process creates/leads the barrier, 0 otherwise
// num_procs: total number of processes (only used by leader)
extern void pi_barrier_init(const char* uri, int is_leader, int num_procs);

// wait at barrier (blocking) - should be called each clock cycle
// return: current synchronized cycle count via output parameter
extern void pi_barrier_wait(svBitVecVal* cycle_out);

// get current synchronized cycle count without waiting
extern void pi_barrier_get_cycle(svBitVecVal* cycle_out);

// close barrier
extern void pi_barrier_close(void);

// check if barrier is ready (all processes registered)
extern int pi_barrier_ready(void);

// update num procs (for dynamic RM loading)
extern void pi_barrier_set_num_procs(int num_procs);

// get num procs
extern int pi_barrier_get_num_procs(void);

#ifdef __cplusplus
}
#endif

// global barrier procs (one per process)
static cycle_barrier* g_barrier = nullptr;

void pi_barrier_init(const char* uri, int is_leader, int num_procs) {
    if (g_barrier != nullptr) {
        fprintf(stderr, "pi_barrier_init: barrier already initialized\n");
        return;
    }

    g_barrier = barrier_open(uri, is_leader != 0, (uint32_t)num_procs);

    if (g_barrier == nullptr) {
        fprintf(stderr, "pi_barrier_init: failed to open barrier at %s\n", uri);
        exit(1);
    }
}

void pi_barrier_wait(svBitVecVal* cycle_out) {
    if (g_barrier == nullptr) {
        fprintf(stderr, "pi_barrier_wait: barrier not initialized\n");
        exit(1);
    }
    uint64_t cycle = barrier_wait(g_barrier);
    memcpy(cycle_out, &cycle, sizeof(uint64_t));
}

void pi_barrier_get_cycle(svBitVecVal* cycle_out) {
    if (g_barrier == nullptr) {
        fprintf(stderr, "pi_barrier_get_cycle: barrier not initialized\n");
        exit(1);
    }

    uint64_t cycle = barrier_get_cycle(g_barrier);
    memcpy(cycle_out, &cycle, sizeof(uint64_t));
}

void pi_barrier_close(void) {
    if (g_barrier != nullptr) {
        barrier_close(g_barrier);
        g_barrier = nullptr;
    }
}

int pi_barrier_ready(void) {
    if (g_barrier == nullptr) {
        return 0;
    }
    return barrier_all_ready(g_barrier) ? 1 : 0;
}

void pi_barrier_set_num_procs(int num_procs) {
    if (g_barrier == nullptr) {
        fprintf(stderr, "pi_barrier_set_num_procs: barrier not initialized\n");
        return;
    }
    barrier_set_num_processes(g_barrier, (uint32_t)num_procs);
}

int pi_barrier_get_num_procs(void) {
    if (g_barrier == nullptr) {
        return 0;
    }
    return (int)barrier_get_num_processes(g_barrier);
}
