// exchange_core.cpp -- pybind11 bindings for the llcpp exchange.
// Exposes order submission, book inspection, execution reports, market data,
// and performance stats to Python for functional/behavioral testing.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <vector>
#include <string>
#include <sstream>
#include <filesystem>

#include "matcher/matching_engine.h"
#include "matcher/me_order_book.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"
#include "latency_tracker.h"

namespace py = pybind11;

using namespace Common;
using namespace Exchange;

// ---------------------------------------------------------------------------
// TestExchange: a self-contained harness that owns the queues, engine, and
// order books. Python drives it synchronously -- no background threads.
// ---------------------------------------------------------------------------

class TestExchange {
public:
    static constexpr const char *LOG_DIR = "/tmp/llcpp";

    TestExchange()
        : req_q_(ME_MAX_CLIENT_UPDATES),
          resp_q_(ME_MAX_CLIENT_UPDATES),
          upd_q_(ME_MAX_MARKET_UPDATES),
          logger_(initLogDir() + "/exchange_python_test.log"),
          engine_(&req_q_, &resp_q_, &upd_q_,
                  std::string(LOG_DIR) + "/exchange_matching_engine.log") {}

private:
    // Ensure log directory exists, return it as a string.
    static auto initLogDir() -> std::string {
        std::filesystem::create_directories(LOG_DIR);
        return LOG_DIR;
    }

public:

    // -- Order submission (same path as the Order Gateway Server) -----------

    void add_order(uint32_t client_id, uint64_t order_id, uint32_t ticker_id,
                   const std::string &side, uint64_t price, uint32_t qty) {
        ensure_book(ticker_id);
        books_[ticker_id]->add(
            ClientId{client_id}, OrderId{order_id}, TickerId{ticker_id},
            parse_side(side), Price{price}, Qty{qty});
    }

    void cancel_order(uint32_t client_id, uint64_t order_id, uint32_t ticker_id) {
        ensure_book(ticker_id);
        books_[ticker_id]->cancel(
            ClientId{client_id}, OrderId{order_id}, TickerId{ticker_id});
    }

    // -- Execution reports / client responses -------------------------------

    auto get_responses() -> py::list {
        py::list out;
        while (const auto *r = resp_q_.getNextToRead()) {
            py::dict d;
            d["type"]             = clientResponseTypeToString(r->type_);
            d["client_id"]        = r->client_id_.value;
            d["ticker_id"]        = r->ticker_id_.value;
            d["client_order_id"]  = r->client_order_id_.value;
            d["market_order_id"]  = r->market_order_id_.value;
            d["side"]             = sideToString(r->side_);
            d["price"]            = r->price_.value;
            d["exec_qty"]         = r->exec_qty_.value;
            d["leaves_qty"]       = r->leaves_qty_.value;
            out.append(d);
            resp_q_.updateReadIndex();
        }
        return out;
    }

    // -- Market data output -------------------------------------------------

    auto get_market_updates() -> py::list {
        py::list out;
        while (const auto *u = upd_q_.getNextToRead()) {
            py::dict d;
            d["type"]      = marketUpdateTypeToString(u->type_);
            d["ticker_id"] = u->ticker_id_.value;
            d["side"]      = sideToString(u->side_);
            d["price"]     = u->price_.value;
            d["qty"]       = u->qty_.value;
            d["order_id"]  = u->order_id_.value;
            d["priority"]  = u->priority_.value;
            out.append(d);
            upd_q_.updateReadIndex();
        }
        return out;
    }

    // -- Order book state inspection ----------------------------------------

    struct PriceLevel {
        std::string side;
        uint64_t    price;
        uint32_t    total_qty;
        uint32_t    order_count;
    };

    auto get_bid_levels(uint32_t ticker_id) -> std::vector<PriceLevel> {
        ensure_book(ticker_id);
        return collect_levels(books_[ticker_id], Side::BUY);
    }

    auto get_ask_levels(uint32_t ticker_id) -> std::vector<PriceLevel> {
        ensure_book(ticker_id);
        return collect_levels(books_[ticker_id], Side::SELL);
    }

    auto get_best_bid(uint32_t ticker_id) -> py::object {
        auto levels = get_bid_levels(ticker_id);
        if (levels.empty()) return py::none();
        return py::cast(levels[0].price);
    }

    auto get_best_ask(uint32_t ticker_id) -> py::object {
        auto levels = get_ask_levels(ticker_id);
        if (levels.empty()) return py::none();
        return py::cast(levels[0].price);
    }

    auto get_spread(uint32_t ticker_id) -> py::object {
        auto bb = get_best_bid(ticker_id);
        auto ba = get_best_ask(ticker_id);
        if (bb.is_none() || ba.is_none()) return py::none();
        int64_t spread = py::cast<int64_t>(ba) - py::cast<int64_t>(bb);
        return py::cast(spread);
    }

    // -- Performance stats --------------------------------------------------

    auto get_perf_stats(uint32_t ticker_id) -> py::dict {
        ensure_book(ticker_id);
        auto s = books_[ticker_id]->getPerfStats();
        py::dict d;
        d["count"]            = s.count;
        d["min_ns"]           = s.min_ns;
        d["max_ns"]           = s.max_ns;
        d["mean_ns"]          = s.mean_ns;
        d["p50_ns"]           = s.p50_ns;
        d["p99_ns"]           = s.p99_ns;
        d["p999_ns"]          = s.p999_ns;
        d["throughput_per_sec"] = s.throughput_per_sec;
        return d;
    }

    void reset_perf_stats(uint32_t ticker_id) {
        ensure_book(ticker_id);
        books_[ticker_id]->resetPerfStats();
    }

    auto __repr__() const -> std::string {
        return "TestExchange(tickers=" + std::to_string(ME_MAX_TICKERS) + ")";
    }

private:
    void ensure_book(uint32_t ticker_id) {
        if (!books_[ticker_id]) {
            books_[ticker_id] = new MEOrderBook(
                TickerId{ticker_id}, &logger_, &engine_);
        }
    }

    static auto parse_side(const std::string &s) -> Side {
        if (s == "BUY")  return Side::BUY;
        if (s == "SELL") return Side::SELL;
        throw std::invalid_argument("Invalid side: '" + s + "'. Must be 'BUY' or 'SELL'.");
    }

    auto collect_levels(MEOrderBook *book, Side side) -> std::vector<PriceLevel> {
        std::vector<PriceLevel> levels;
        // Walk the price-level linked list via the hash map and sorted list.
        // We scan all price slots (fast -- only 256 slots).
        for (size_t i = 0; i < ME_MAX_PRICE_LEVELS; ++i) {
            auto *oap = book->getOrdersAtPrice(Price{static_cast<uint64_t>(i)});
            if (oap && oap->side_ == side) {
                uint32_t total_qty = 0;
                uint32_t count = 0;
                auto *order = oap->first_me_order_;
                if (order) {
                    auto *cur = order;
                    do {
                        total_qty += cur->qty_.value;
                        ++count;
                        cur = cur->next_order_;
                    } while (cur != order);
                }
                levels.push_back({sideToString(side), oap->price_.value, total_qty, count});
            }
        }
        // Sort: bids descending, asks ascending.
        if (side == Side::BUY) {
            std::sort(levels.begin(), levels.end(),
                      [](const PriceLevel &a, const PriceLevel &b) { return a.price > b.price; });
        } else {
            std::sort(levels.begin(), levels.end(),
                      [](const PriceLevel &a, const PriceLevel &b) { return a.price < b.price; });
        }
        return levels;
    }

    ClientRequestLFQueue  req_q_;
    ClientResponseLFQueue resp_q_;
    MEMarketUpdateLFQueue upd_q_;
    Logger                logger_;
    MatchingEngine        engine_;
    OrderBookHashMap      books_{};

    // Clean up order books on destruction.
public:
    ~TestExchange() {
        for (auto &b : books_) {
            delete b;
            b = nullptr;
        }
    }
};

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

PYBIND11_MODULE(exchange_core, m) {
    m.doc() = "Python bindings for the llcpp exchange matching engine";

    // -- Side enum --
    py::enum_<Side>(m, "Side")
        .value("BUY",     Side::BUY)
        .value("SELL",    Side::SELL)
        .value("INVALID", Side::INVALID);

    // -- ClientResponseType enum --
    py::enum_<ClientResponseType>(m, "ClientResponseType")
        .value("INVALID",         ClientResponseType::INVALID)
        .value("ACCEPTED",        ClientResponseType::ACCEPTED)
        .value("CANCELED",        ClientResponseType::CANCELED)
        .value("FILLED",          ClientResponseType::FILLED)
        .value("CANCEL_REJECTED", ClientResponseType::CANCEL_REJECTED);

    // -- MarketUpdateType enum --
    py::enum_<MarketUpdateType>(m, "MarketUpdateType")
        .value("INVALID", MarketUpdateType::INVALID)
        .value("ADD",     MarketUpdateType::ADD)
        .value("MODIFY",  MarketUpdateType::MODIFY)
        .value("CANCEL",  MarketUpdateType::CANCEL)
        .value("TRADE",   MarketUpdateType::TRADE);

    // -- PriceLevel (book inspection) --
    py::class_<TestExchange::PriceLevel>(m, "PriceLevel")
        .def_readonly("side",        &TestExchange::PriceLevel::side)
        .def_readonly("price",       &TestExchange::PriceLevel::price)
        .def_readonly("total_qty",   &TestExchange::PriceLevel::total_qty)
        .def_readonly("order_count", &TestExchange::PriceLevel::order_count)
        .def("__repr__", [](const TestExchange::PriceLevel &l) {
            return "PriceLevel(side=" + l.side +
                   ", price=" + std::to_string(l.price) +
                   ", total_qty=" + std::to_string(l.total_qty) +
                   ", order_count=" + std::to_string(l.order_count) + ")";
        });

    // -- PerfStats --
    py::class_<Common::PerfStats>(m, "PerfStats")
        .def_readonly("count",            &Common::PerfStats::count)
        .def_readonly("min_ns",           &Common::PerfStats::min_ns)
        .def_readonly("max_ns",           &Common::PerfStats::max_ns)
        .def_readonly("mean_ns",          &Common::PerfStats::mean_ns)
        .def_readonly("p50_ns",           &Common::PerfStats::p50_ns)
        .def_readonly("p99_ns",           &Common::PerfStats::p99_ns)
        .def_readonly("p999_ns",          &Common::PerfStats::p999_ns)
        .def_readonly("throughput_per_sec", &Common::PerfStats::throughput_per_sec)
        .def("__repr__", [](const Common::PerfStats &s) {
            std::ostringstream os;
            os << "PerfStats(count=" << s.count
               << ", p50=" << s.p50_ns << "ns"
               << ", p99=" << s.p99_ns << "ns"
               << ", max=" << s.max_ns << "ns)";
            return os.str();
        });

    // -- TestExchange (main harness) --
    py::class_<TestExchange>(m, "TestExchange")
        .def(py::init<>())
        .def("add_order",          &TestExchange::add_order,
             py::arg("client_id"), py::arg("order_id"), py::arg("ticker_id"),
             py::arg("side"), py::arg("price"), py::arg("qty"),
             "Submit a new limit order to the matching engine.")
        .def("cancel_order",       &TestExchange::cancel_order,
             py::arg("client_id"), py::arg("order_id"), py::arg("ticker_id"),
             "Cancel an existing order.")
        .def("get_responses",      &TestExchange::get_responses,
             "Drain and return all pending client responses (execution reports).")
        .def("get_market_updates", &TestExchange::get_market_updates,
             "Drain and return all pending market data updates.")
        .def("get_bid_levels",     &TestExchange::get_bid_levels,
             py::arg("ticker_id") = 0,
             "Return bid price levels sorted descending by price.")
        .def("get_ask_levels",     &TestExchange::get_ask_levels,
             py::arg("ticker_id") = 0,
             "Return ask price levels sorted ascending by price.")
        .def("get_best_bid",       &TestExchange::get_best_bid,
             py::arg("ticker_id") = 0,
             "Return best bid price or None if no bids.")
        .def("get_best_ask",       &TestExchange::get_best_ask,
             py::arg("ticker_id") = 0,
             "Return best ask price or None if no asks.")
        .def("get_spread",         &TestExchange::get_spread,
             py::arg("ticker_id") = 0,
             "Return bid-ask spread or None if either side is empty.")
        .def("get_perf_stats",     &TestExchange::get_perf_stats,
             py::arg("ticker_id") = 0,
             "Return C++ LatencyTracker performance stats for the given ticker.")
        .def("reset_perf_stats",   &TestExchange::reset_perf_stats,
             py::arg("ticker_id") = 0,
             "Reset the C++ LatencyTracker for the given ticker.")
        .def("__repr__",           &TestExchange::__repr__);
}
