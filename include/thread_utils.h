#pragma once

#include <iostream>
#include <atomic>
#include <thread>
#include <utility>
#include <unistd.h>
#include <sys/syscall.h>

namespace Common {
    /// Set affinity for current thread to be pinned to the provided core_id.
    inline auto setThreadCore(int core_id) noexcept {
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);

        return (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0);
    }

    /// Creates a thread instance, sets affinity on it, assigns it a name and
    /// passes the function to be run on that thread as well as the arguments to the function.
    ///
    /// Notes:
    ///   - Everything is captured BY VALUE. The previous version captured by
    ///     reference and parked the parent in sleep_for(1s) hoping the thread
    ///     read the captures before they went out of scope — a race and a
    ///     full second of startup latency per thread (13+ seconds across the
    ///     exchange + client processes).
    ///   - Affinity failure is a warning, not a process exit: on a small VM
    ///     the requested core may simply not exist, and running unpinned is
    ///     the right degradation (latency tails get noisier; nothing breaks).
    template<typename T, typename... A>
    inline auto createAndStartThread(int core_id, const std::string &name, T &&func, A &&... args) noexcept {
        auto t = new std::thread([core_id, name,
                                  func = std::forward<T>(func),
                                  ... args = std::forward<A>(args)]() mutable {
          if (core_id >= 0) {
            if (!setThreadCore(core_id)) {
              std::cerr << "WARN: failed to pin " << name << " " << pthread_self()
                        << " to core " << core_id << " — running unpinned." << std::endl;
            } else {
              std::cerr << "Set core affinity for " << name << " " << pthread_self()
                        << " to " << core_id << std::endl;
            }
          }

          std::move(func)((std::move(args))...);
        });

        return t;
    }
}
