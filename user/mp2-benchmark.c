// clang-format off
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
// clang-format on

#define NULL 0

int min(int a, int b) {
  return (a < b) ? a : b;  // Return the minimum of two integers
}

// Return the minumum time taken to complete a workload
int benchmark_workload(int iterations, int workload) {
  int iteration, start_time, min_time;
  min_time = __INT_MAX__;
  for (iteration = 0; iteration < iterations; iteration++) {
    start_time = uptime();                            // Get the start time
    simulate_work(workload);                          // Simulate some work
    min_time = min(min_time, uptime() - start_time);  // Aggregate the time taken
  }
  return min_time;
}

int main(int argc, char *argv[]) {
  int workload;
  for (workload = 1; workload <= (1 << 30); workload *= 2) {
    if (benchmark_workload(10, workload) >= 10) {
      // Found a workload that takes at least 10 ticks
      break;
    }
  }
  proclog(workload);  // Log the workload
  exit(0);
}
