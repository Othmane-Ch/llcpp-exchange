
#include "matching_engine.h"

namespace Exchange {

MatchingEngine::MatchingEngine(ClientRequestLFQueue  *client_requests,
                               ClientResponseLFQueue *client_responses,
                               MEMarketUpdateLFQueue *market_updates,
                               const std::string     &log_file)
    : incoming_requests_(client_requests),
      outgoing_responses_(client_responses),
      market_updates_(market_updates),
      logger_(log_file) {
    // Order books are created lazily in processClientRequest to avoid
    // allocating ~2GB per book upfront for all ME_MAX_TICKERS.
    ticker_order_books_.fill(nullptr);
    logger_.log("%:% %() % MatchingEngine created.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
}

MatchingEngine::~MatchingEngine() {
    stop();

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s); // allow detached thread to exit

    incoming_requests_  = nullptr;
    outgoing_responses_ = nullptr;
    market_updates_     = nullptr;

    for (auto &ob : ticker_order_books_) {
        delete ob;
        ob = nullptr;
    }
}

auto MatchingEngine::start() -> void {
    running_ = true;
    auto *t = Common::createAndStartThread(
        -1, "Exchange/MatchingEngine", [this]() { run(); });
    ASSERT(t != nullptr, "Failed to start MatchingEngine thread.");
    t->detach();
    delete t;
    logger_.log("%:% %() % MatchingEngine started.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
}

auto MatchingEngine::stop() -> void {
    logger_.log("%:% %() % MatchingEngine stopping.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
    running_ = false;

    if (incoming_requests_) {
        auto *slot = incoming_requests_->getNextToWriteTo();
        new (slot) MEClientRequest{ClientRequestType::SHUTDOWN,
                                   ClientId{}, TickerId{}, OrderId{},
                                   Side::INVALID, Price{}, Qty{}};
        incoming_requests_->updateWriteIndex();
    }
}

} // namespace Exchange
