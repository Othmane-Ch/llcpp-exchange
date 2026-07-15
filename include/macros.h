#pragma once

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace Common {
    [[noreturn]] inline auto fatalExit(const std::string &msg) noexcept -> void {
        std::cerr << "FATAL : " << msg << std::endl;
        exit(EXIT_FAILURE);
    }
}

// ASSERT / FATAL are macros (not inline functions) so the message expression
// — typically a heap-allocating std::string concatenation — is evaluated
// ONLY on the failure path. The previous inline-function versions built the
// message on every call, which put a malloc + string format on hot paths
// like MemPool::allocate() and LFQueue::updateReadIndex().
// The condition itself stays active in all build types: one predicted branch
// is cheap insurance on an exchange.
#define ASSERT(cond, msg)                                  \
    do {                                                   \
        if (UNLIKELY(!(cond))) {                           \
            Common::fatalExit(msg);                        \
        }                                                  \
    } while (false)

#define FATAL(msg) Common::fatalExit(msg)
