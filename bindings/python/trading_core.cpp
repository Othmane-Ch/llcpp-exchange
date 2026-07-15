// trading_core.cpp -- pybind11 bindings for the client-side Trading library.
//
// Exposes Trading::MarketOrderBook through a small harness so Python tests
// can drive the client-side book reconstruction without spinning up sockets.
// The MarketOrderBook is fed synthetic MEMarketUpdate inputs and its BBO /
// price levels are reflected back into Python as plain dicts / lists.
//
// Keep this file small: it mirrors exchange_core.cpp in shape but targets
// the trading side. No threads, no networking.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "logging.h"
#include "market_order_book.h"
#include "market_update.h"

namespace py = pybind11;

using namespace Common;
using Exchange::MEMarketUpdate;
using Exchange::MarketUpdateType;
using Trading::MarketOrderBook;
using Trading::MarketOrdersAtPrice;

// ---------------------------------------------------------------------------
// TestMarketOrderBook: own a Logger + MarketOrderBook, expose simple ops.
// ---------------------------------------------------------------------------

class TestMarketOrderBook {
public:
    static constexpr const char *LOG_DIR = "/tmp/llcpp";

    explicit TestMarketOrderBook(uint32_t ticker_id = 0)
        : logger_(initLogDir() + "/trading_python_test.log"),
          book_(new MarketOrderBook(TickerId{ticker_id}, &logger_)) {}

    ~TestMarketOrderBook() = default;

    // -- Feeding updates ----------------------------------------------------

    void on_add(const std::string &side, uint64_t price, uint32_t qty,
                uint64_t order_id, uint64_t priority = 1) {
        MEMarketUpdate u{};
        u.type_      = MarketUpdateType::ADD;
        u.ticker_id_ = TickerId{0};
        u.side_      = parse_side(side);
        u.price_     = Price{price};
        u.qty_       = Qty{qty};
        u.order_id_  = OrderId{order_id};
        u.priority_  = Priority{priority};
        book_->onMarketUpdate(&u);
    }

    void on_modify(const std::string &side, uint64_t price, uint32_t new_qty,
                   uint64_t order_id) {
        MEMarketUpdate u{};
        u.type_      = MarketUpdateType::MODIFY;
        u.ticker_id_ = TickerId{0};
        u.side_      = parse_side(side);
        u.price_     = Price{price};
        u.qty_       = Qty{new_qty};
        u.order_id_  = OrderId{order_id};
        book_->onMarketUpdate(&u);
    }

    void on_cancel(const std::string &side, uint64_t price, uint64_t order_id) {
        MEMarketUpdate u{};
        u.type_      = MarketUpdateType::CANCEL;
        u.ticker_id_ = TickerId{0};
        u.side_      = parse_side(side);
        u.price_     = Price{price};
        u.order_id_  = OrderId{order_id};
        book_->onMarketUpdate(&u);
    }

    void on_trade(const std::string &side, uint64_t price, uint32_t qty) {
        MEMarketUpdate u{};
        u.type_      = MarketUpdateType::TRADE;
        u.ticker_id_ = TickerId{0};
        u.side_      = parse_side(side);
        u.price_     = Price{price};
        u.qty_       = Qty{qty};
        book_->onMarketUpdate(&u);
    }

    void on_clear() {
        MEMarketUpdate u{};
        u.type_ = MarketUpdateType::CLEAR;
        book_->onMarketUpdate(&u);
    }

    // -- Inspection --------------------------------------------------------

    auto get_bbo() -> py::dict {
        const auto &b = book_->getBBO();
        py::dict d;
        d["bid_price"] = b.bid_price_.isValid() ? py::cast(b.bid_price_.value) : py::none();
        d["ask_price"] = b.ask_price_.isValid() ? py::cast(b.ask_price_.value) : py::none();
        d["bid_qty"]   = b.bid_qty_.value;
        d["ask_qty"]   = b.ask_qty_.value;
        return d;
    }

    auto get_bid_levels() -> py::list {
        return walk_side(book_->getBidsByPrice());
    }

    auto get_ask_levels() -> py::list {
        return walk_side(book_->getAsksByPrice());
    }

    auto has_order(uint64_t order_id) -> bool {
        return book_->getOrder(OrderId{order_id}) != nullptr;
    }

    auto __repr__() const -> std::string {
        return "TestMarketOrderBook()";
    }

private:
    static auto initLogDir() -> std::string {
        std::filesystem::create_directories(LOG_DIR);
        return LOG_DIR;
    }

    static auto parse_side(const std::string &s) -> Side {
        if (s == "BUY")  return Side::BUY;
        if (s == "SELL") return Side::SELL;
        throw std::invalid_argument("Invalid side '" + s + "' -- must be BUY or SELL");
    }

    static auto walk_side(const MarketOrdersAtPrice *head) -> py::list {
        py::list out;
        if (!head) return out;
        const auto *cur = head;
        // Sorted circular DLL -- walk forward until we loop back.
        do {
            py::dict level;
            level["price"] = cur->price_.value;
            level["side"]  = sideToString(cur->side_);

            uint32_t total_qty = 0;
            uint32_t count = 0;
            const auto *order = cur->first_mkt_order_;
            if (order) {
                const auto *o = order;
                do {
                    total_qty += o->qty_.value;
                    ++count;
                    o = o->next_order_;
                } while (o != order);
            }
            level["total_qty"]   = total_qty;
            level["order_count"] = count;
            out.append(level);

            cur = cur->next_entry_;
        } while (cur && cur != head);
        return out;
    }

    Logger                            logger_;
    std::unique_ptr<MarketOrderBook>  book_;
};

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

PYBIND11_MODULE(trading_core, m) {
    m.doc() = "Python bindings for the llcpp client-side trading library";

    py::class_<TestMarketOrderBook>(m, "TestMarketOrderBook")
        .def(py::init<uint32_t>(), py::arg("ticker_id") = 0)
        .def("on_add",     &TestMarketOrderBook::on_add,
             py::arg("side"), py::arg("price"), py::arg("qty"),
             py::arg("order_id"), py::arg("priority") = 1,
             "Apply an ADD market update.")
        .def("on_modify",  &TestMarketOrderBook::on_modify,
             py::arg("side"), py::arg("price"), py::arg("new_qty"),
             py::arg("order_id"),
             "Apply a MODIFY market update (updates resting qty).")
        .def("on_cancel",  &TestMarketOrderBook::on_cancel,
             py::arg("side"), py::arg("price"), py::arg("order_id"),
             "Apply a CANCEL market update.")
        .def("on_trade",   &TestMarketOrderBook::on_trade,
             py::arg("side"), py::arg("price"), py::arg("qty"),
             "Apply a TRADE market update (informational only).")
        .def("on_clear",   &TestMarketOrderBook::on_clear,
             "Apply a CLEAR market update (wipes the entire book).")
        .def("get_bbo",        &TestMarketOrderBook::get_bbo,
             "Return current top-of-book as a dict.")
        .def("get_bid_levels", &TestMarketOrderBook::get_bid_levels,
             "Return bid levels walked head-to-tail (descending).")
        .def("get_ask_levels", &TestMarketOrderBook::get_ask_levels,
             "Return ask levels walked head-to-tail (ascending).")
        .def("has_order",      &TestMarketOrderBook::has_order,
             py::arg("order_id"),
             "Return True iff the given market order id is resting on the book.")
        .def("__repr__",       &TestMarketOrderBook::__repr__);
}
