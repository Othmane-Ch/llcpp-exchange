//
// opt_logging.h — optimized variant of Common::Logger.
//
// Identical to common/logging.h except:
//   - Adds STRING = 9 to LogType.
//   - Adds `char s[256]` to LogElement.u_.
//   - pushValue(const char *) now block-copies up to 255 bytes via
//     strncpy and enqueues a single STRING element, instead of
//     enqueueing one CHAR element per byte.
//   - flushQueue() handles STRING by writing the buffer directly.
//
// Mechanism: the baseline `pushValue(const char *)` loops over each
// character and calls `pushValue(char)` per byte. For an N-byte string
// that is N function calls, N LFQueue push operations (each with one
// atomic increment on next_write_index_ and num_elements_), and N
// branches on the consumer side. The block-copy variant is one push +
// one buffered write, eliminating N-1 atomic increments per string.
//
// 256 bytes was chosen as the LogElement string-buffer size: large
// enough for typical formatted log lines in this codebase
// (MEMarketUpdate::toString is bounded at 320 bytes elsewhere, and
// most log lines are <128 chars) while staying inside a few cache
// lines per element. The trade-off is wasted memory in non-STRING
// elements — the union pads up to the max member size.
//

#pragma once

#include <cstring>
#include <fstream>
#include <iosfwd>
#include <thread>

#include "thread_utils.h"
#include "lq_free.h"
#include "types.h"

namespace OptCommon
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
        LogType type_ = LogType::CHAR;
        union {
            char c;
            int i; long li; long long ll;
            unsigned u; unsigned long ul; unsigned long long ull;
            float f; double d;
            char s[256];
        } u_;
    };

    class OptLogger final
    {
    public:

        auto flushQueue() noexcept -> void
        {
            while (running_.load() || queue_.size() > 0)
            {
                const auto log_elem = queue_.getNextToRead();
                if (log_elem != nullptr)
                {
                    switch (log_elem->type_)
                    {
                        case LogType::CHAR:
                            file_ << log_elem->u_.c;
                            break;
                        case LogType::INTEGER:
                            file_ << log_elem->u_.i;
                            break;
                        case LogType::LONG_INTEGER:
                            file_ << log_elem->u_.li;
                            break;
                        case LogType::LONG_LONG_INTEGER:
                            file_ << log_elem->u_.ll;
                            break;
                        case LogType::UNSIGNED_INTEGER:
                            file_ << log_elem->u_.u;
                            break;
                        case LogType::UNSIGNED_LONG_INTEGER:
                            file_ << log_elem->u_.ul;
                            break;
                        case LogType::UNSIGNED_LONG_LONG_INTEGER:
                            file_ << log_elem->u_.ull;
                            break;
                        case LogType::FLOAT:
                            file_ << log_elem->u_.f;
                            break;
                        case LogType::DOUBLE:
                            file_ << log_elem->u_.d;
                            break;
                        case LogType::STRING:
                            // Block-write the buffer (null-terminated by strncpy).
                            file_ << log_elem->u_.s;
                            break;
                    }
                    queue_.updateReadIndex();
                }
                using namespace std::literals::chrono_literals;
                std::this_thread::sleep_for(1ms);
            }
        }

        explicit OptLogger(const std::string &fname)
            : file_name_(fname), queue_(Common::LOG_QUEUE_SIZE)
        {
            file_.open(file_name_);
            ASSERT(file_.is_open(), "Could not open log file:" + file_name_);
            logger_thread_ = Common::createAndStartThread(-1, "OptCommon/Logger " + file_name_,
                                                          [this]() { flushQueue(); });
            ASSERT(logger_thread_ != nullptr, "Failed to start OptLogger thread.");
        }

        auto pushValue(const LogElement &log_elem) noexcept
        {
            auto dest = queue_.getNextToWriteTo();
            *dest = log_elem;
            queue_.updateWriteIndex();
        }

        auto pushValue(const char c) noexcept
        {
            LogElement log_elem;
            log_elem.type_ = LogType::CHAR;
            log_elem.u_.c = c;
            pushValue(log_elem);
        }

        // Optimized: block-copy the string into the LogElement instead of
        // pushing one CHAR element per byte.
        auto pushValue(const char *value) noexcept
        {
            LogElement l{LogType::STRING, {.s = {}}};
            // strncpy does NOT null-terminate when the source length equals or
            // exceeds the destination buffer. The `- 1` reserves the last byte
            // for a guaranteed null terminator.
            std::strncpy(l.u_.s, value, sizeof(l.u_.s) - 1);
            pushValue(l);
        }

        auto pushValue(const std::string &str) noexcept
        {
            pushValue(str.c_str());
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
            while (*s) {
                if (*s == '%') {
                    if (UNLIKELY(*(s + 1) == '%')) {
                        ++s;
                    } else {
                        pushValue(value);
                        log(s + 1, args...);
                        return;
                    }
                }
                pushValue(*s++);
            }
            FATAL("extra arguments provided to log()");
        }

        auto log(const char *s) noexcept {
            while (*s) {
                if (*s == '%') {
                    if (UNLIKELY(*(s + 1) == '%')) {
                        ++s;
                    } else {
                        FATAL("missing arguments to log()");
                    }
                }
                pushValue(*s++);
            }
        }

        ~OptLogger()
        {
            std::cerr << "Shutting down OptLogger for file:" << file_name_ << std::endl;
            while (queue_.size())
            {
                using namespace std::literals::chrono_literals;
                std::this_thread::sleep_for(1s);
            }
            running_.store(false);
            logger_thread_->join();
            if (file_.is_open())
            {
                file_.close();
            }
        }

        OptLogger() = delete;
        OptLogger(const OptLogger &) = delete;
        OptLogger &operator=(const OptLogger &) = delete;
        OptLogger(OptLogger &&) = delete;
        OptLogger &operator=(OptLogger &&) = delete;

    private:
        const std::string file_name_;
        std::ofstream file_;
        Common::LFQueue<LogElement> queue_;
        std::atomic<bool> running_ = {true};
        std::thread * logger_thread_ = nullptr;
    };
}
