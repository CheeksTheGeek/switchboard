// variant of testbench.cc variant that uses barrier synchronization to ensure all
// processes advance cycle-by-cycle together, achieving true cycle-accuracy.
#include <signal.h> // mayb neeed for ctrl+c
#include <memory>
#include <cmath>
#include <cinttypes>
#include <iostream>
#include <sstream>
#include <string>
#include <verilated.h>
#include "Vtestbench.h"
#include "switchboard.hpp"
#include "../cpp/barrier_sync.h"
double sc_time_stamp() {
    return 0;
}

static volatile int got_sigint = 0;

void sigint_handler(int unused) {
    got_sigint = 1;
}

std::string extract_plusarg_value(const char* match, const char* name) {
    if (match) {
        std::string full = std::string(match);
        std::string prefix = "+" + std::string(name) + "=";
        size_t len = prefix.size();
        if ((full.size() >= (len + 1)) && (full.substr(0, len - 1) == prefix.substr(0, len - 1))) {
            return std::string(match).substr(len);
        }
    }
    return "";
}

template <typename T> void parse_plusarg(const char* match, const char* name, T& result) {
    std::string value = extract_plusarg_value(match, name);
    if (value != "") {
        std::istringstream iss(value);
        iss >> result;
    }
}

std::string get_plusarg_string(VerilatedContext* contextp, const char* name) {
    const char* match = contextp->commandArgsPlusMatch(name);
    return extract_plusarg_value(match, name);
}

int main(int argc, char** argv, char** env) {
    if (false && argc && argv && env) {}

    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->traceEverOn(true);
    contextp->commandArgs(argc, argv);

    const std::unique_ptr<Vtestbench> top{new Vtestbench{contextp.get(), "TOP"}};

    double period = 10e-9;
    const char* period_match = contextp->commandArgsPlusMatch("period");
    parse_plusarg<double>(period_match, "period", period);
    std::string barrier_uri = get_plusarg_string(contextp.get(), "barrier_uri");
    int barrier_leader = 0;
    const char* leader_match = contextp->commandArgsPlusMatch("barrier_leader");
    parse_plusarg<int>(leader_match, "barrier_leader", barrier_leader);
    int barrier_procs = 2;
    const char* procs_match = contextp->commandArgsPlusMatch("barrier_procs");
    parse_plusarg<int>(procs_match, "barrier_procs", barrier_procs);
    uint64_t max_cycles = 0;
    const char* cycles_match = contextp->commandArgsPlusMatch("max_cycles");
    parse_plusarg<uint64_t>(cycles_match, "max_cycles", max_cycles);
    uint64_t iperiod = std::round(period * std::pow(10.0, -1.0 * contextp->timeprecision()));
    uint64_t duration0 = iperiod / 2;
    uint64_t duration1 = iperiod - duration0;
    cycle_barrier* barrier = nullptr;
    if (!barrier_uri.empty()) {
        barrier = barrier_open(barrier_uri.c_str(), barrier_leader != 0, barrier_procs);
        if (!barrier) {
            fprintf(stderr, "Failed to open barrier at %s\n", barrier_uri.c_str());
            return 1;
        }
        printf("[testbench_sync] Barrier sync enabled: uri=%s, leader=%d, procs=%d\n",
               barrier_uri.c_str(), barrier_leader, barrier_procs);
    }

    top->clk = 0;
    top->eval();

    signal(SIGINT, sigint_handler);
    double start_delay_value = -1;
    const char* delay_match = contextp->commandArgsPlusMatch("start-delay");
    parse_plusarg<double>(delay_match, "start-delay", start_delay_value);
    start_delay(start_delay_value);
    // sim loop
    // we need to use this to eliminate race conditions in queue data path:
    // in phase 1, we eval to produce outputs, barrier wait (guarantees data availability)
    // in phase 2, we eval to consume inputs, barrier wait (guarantees data stability)
    uint64_t cycle = 0;
    while (!(contextp->gotFinish() || got_sigint)) {
        // Check max cycles limit
        if (max_cycles > 0 && cycle >= max_cycles) {
            printf("[testbench_sync] Reached max_cycles limit: %" PRIu64 "\n", max_cycles);
            break;
        }
        // evaluate & send : first eval to produce outputs based on current state,
        // then (implicitly) trigger dpi/vpi calls to produce outputs
        top->eval();

        // Wait for all processes to finish producing outputs.
        // This guarantees all data is written before anyone reads.
        if (barrier) {
            barrier_wait(barrier);
        }

        // Rising and falling clock edges
        contextp->timeInc(duration0);
        top->clk = 1;
        top->eval();
        contextp->timeInc(duration1);
        top->clk = 0;
        top->eval();

        cycle++;
    }
    if (barrier) {
        barrier_close(barrier);
    }
    top->final();

    printf("[testbench_sync] Simulation ended after %" PRIu64 " cycles\n", cycle);

    return 0;
}
