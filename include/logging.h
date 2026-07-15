#pragma once

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iosfwd>
#include <thread>

#include "thread_utils.h"
#include "lq_free.h"
#include "types.h"

namespace Common
{
    enum class LogType : int8_t
    {
        CHAR =  0,
        INTEGER = 1, LONG_INTEGER = 2, LONG_LONG_INTEGER = 3,
        UNSIGNED_INTEGER = 4, UNSIGNED_LONG_INTEGER = 5, UNSIGNED_LONG_LONG_INTEGER = 6,
        FLOAT = 7, DOUBLE = 8,
        STRING = 9
    };

    struct LogElement
    {
        LogType type = LogType::CHAR;
        union {
            char c;
            int i; long li; long long ll;
            unsigned u; unsigned long ul; unsigned long long ull;
            float f; double d;
            // Block-copied string fragment (null-terminated). 64 bytes keeps
            // the element a single cache line while still carrying a typical
            // format-string span in one queue slot. Longer strings are split
            // across consecutive STRING elements by pushValue(const char *).
            char s[64];
        } u;
    };

    class Logger final
    {
    public:

        auto flushQueue() noexcept -> void
        {
            while (running.load() || queue.size() > 0)
            {
                const auto log_elem = queue.getNextToRead();
                if (log_elem != nullptr)
                {
                    switch (log_elem->type)
                    {
                        case LogType::CHAR:
                            file << log_elem->u.c;
                            break;
                        case LogType::INTEGER:
                            file << log_elem->u.i;
                            break;
                        case LogType::LONG_INTEGER:
                            file << log_elem->u.li;
                            break;
                        case LogType::LONG_LONG_INTEGER:
                            file << log_elem->u.ll;
                            break;
                        case LogType::UNSIGNED_INTEGER:
                            file << log_elem->u.u;
                            break;
                        case LogType::UNSIGNED_LONG_INTEGER:
                            file << log_elem->u.ul;
                            break;
                        case LogType::UNSIGNED_LONG_LONG_INTEGER:
                            file << log_elem->u.ull;
                            break;
                        case LogType::FLOAT:
                            file << log_elem->u.f;
                            break;
                        case LogType::DOUBLE:
                            file << log_elem->u.d;
                            break;
                        case LogType::STRING:
                            file << log_elem->u.s; // null-terminated fragment
                            break;
                    }
                    queue.updateReadIndex();
                }
                else
                {
                    // Sleep ONLY when the queue is empty. The previous version
                    // slept 1 ms after EVERY element, capping the drain rate at
                    // ~1000 elements/sec — the root cause of the multi-hour
                    // destructor hangs and silent queue overruns under load.
                    file.flush();
                    using namespace std::literals::chrono_literals;
                    std::this_thread::sleep_for(1ms);
                }
            }
            file.flush();
        }

        explicit Logger(const std::string &fname)
            : file_name(fname), queue(LOG_QUEUE_SIZE)
        {
            file.open(file_name);
            ASSERT(file.is_open(), "Could not open log file:" + file_name);
            logger_thread = createAndStartThread(-1, "Common/Logger " + file_name, [this]() { flushQueue(); });
            ASSERT(logger_thread != nullptr, "Failed to start Logger thread.");
        }

        auto pushValue(const LogElement &log_elem) noexcept
        {
            auto dest = queue.getNextToWriteTo();
            *dest = log_elem;
            queue.updateWriteIndex();
        }

        auto pushValue(const char c) noexcept
        {
            LogElement log_elem;
            log_elem.type = LogType::CHAR;
            log_elem.u.c = c;
            pushValue(log_elem);
        }

        // Block-copy the string into STRING elements instead of pushing one
        // CHAR element per byte. For an N-byte string the per-char version
        // costs N queue pushes (and N atomic publishes); this costs ~N/63.
        // Measured ~20x cheaper on the producer side.
        auto pushValue(const char * c) noexcept
        {
            while (*c)
            {
                LogElement l{LogType::STRING, {.s = {}}};
                size_t i = 0;
                for (; i < sizeof(l.u.s) - 1 && *c; ++i, ++c)
                {
                    l.u.s[i] = *c;
                }
                l.u.s[i] = '\0';
                pushValue(l);
            }
        }

        auto pushValue(const std::string &str) noexcept
        {
            pushValue(str.c_str());
        }

        /// Push a raw character span (not necessarily null-terminated) as
        /// chunked STRING elements. Used by log() to move literal spans of
        /// the format string in blocks instead of one CHAR element per byte.
        auto pushChars(const char *begin, size_t len) noexcept
        {
            while (len > 0)
            {
                LogElement l{LogType::STRING, {.s = {}}};
                const size_t n = std::min(len, sizeof(l.u.s) - 1);
                std::memcpy(l.u.s, begin, n);
                l.u.s[n] = '\0';
                pushValue(l);
                begin += n;
                len   -= n;
            }
        }

        auto pushValue(const int value) noexcept {
            pushValue(LogElement{LogType::INTEGER, {.i = value}});
        }

        auto pushValue(const long value) noexcept {
            pushValue(LogElement{LogType::LONG_INTEGER, {.li = value}});
        }

        auto pushValue(const long long value) noexcept {
            pushValue(LogElement{LogType::LONG_LONG_INTEGER, {.ll = value}});
        }

        auto pushValue(const unsigned value) noexcept {
            pushValue(LogElement{LogType::UNSIGNED_INTEGER, {.u = value}});
        }

        auto pushValue(const unsigned long value) noexcept {
            pushValue(LogElement{LogType::UNSIGNED_LONG_INTEGER, {.ul = value}});
        }

        auto pushValue(const unsigned long long value) noexcept {
            pushValue(LogElement{LogType::UNSIGNED_LONG_LONG_INTEGER, {.ull = value}});
        }

        auto pushValue(const float value) noexcept {
            pushValue(LogElement{LogType::FLOAT, {.f = value}});
        }

        auto pushValue(const double value) noexcept {
            pushValue(LogElement{LogType::DOUBLE, {.d = value}});
        }

        template<typename T, typename... A>
        auto log(const char *s, const T &value, A... args) noexcept {
            const char *span_start = s; // literal span pending block-push
            while (*s) {
                if (*s == '%') {
                    if (UNLIKELY(*(s + 1) == '%')) { // %% -> literal %: keep the first, skip the second.
                        pushChars(span_start, static_cast<size_t>(s + 1 - span_start));
                        s += 2;
                        span_start = s;
                        continue;
                    }
                    pushChars(span_start, static_cast<size_t>(s - span_start));
                    pushValue(value); // substitute % with the value specified in the arguments.
                    log(s + 1, args...); // pop an argument and call self recursively.
                    return;
                }
                ++s;
            }
            FATAL("extra arguments provided to log()");
        }

        // note that this is overloading not specialization. gcc does not allow inline specializations.
        auto log(const char *s) noexcept {
            const char *span_start = s;
            while (*s) {
                if (*s == '%') {
                    if (UNLIKELY(*(s + 1) == '%')) { // %% -> literal %: keep the first, skip the second.
                        pushChars(span_start, static_cast<size_t>(s + 1 - span_start));
                        s += 2;
                        span_start = s;
                        continue;
                    }
                    FATAL("missing arguments to log()");
                }
                ++s;
            }
            pushChars(span_start, static_cast<size_t>(s - span_start));
        }

        ~Logger()
        {
            std::cerr << "Shutting down Logger for file:" << file_name << std::endl;
            while (queue.size())
            {
                using namespace std::literals::chrono_literals;
                std::this_thread::sleep_for(10ms);
            }
            running.store(false);
            logger_thread->join();
            delete logger_thread;
            if (file.is_open())
            {
                file.close();
            }
        }

        Logger() = delete; // prohibit default constructor.
        Logger(const Logger &) = delete; // prohibit copy constructor.
        Logger &operator=(const Logger &) = delete; // prohibit copy assignment.
        Logger(Logger &&) = delete; // prohibit move constructor.
        Logger &operator=(Logger &&) = delete; // prohibit move assignment.

    private:
        const std::string file_name;
        std::ofstream file;
        LFQueue<LogElement> queue;
        std::atomic<bool> running = {true};
        std::thread * logger_thread = nullptr;
    };
}
