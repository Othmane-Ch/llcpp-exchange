
#pragma once

#include <memory>
#include <array>
#include <atomic>
#include <string>

#include "me_order_book.h"
#include "logging.h"
#include "time_utils.h"
#include "thread_utils.h"
#include "me_order.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

using namespace Common;

namespace Exchange {

class MatchingEngine final {
public:
    MatchingEngine(ClientRequestLFQueue  *client_requests,
                   ClientResponseLFQueue *client_responses,
                   MEMarketUpdateLFQueue *market_updates,
                   const std::string     &log_file = "exchange_matching_engine.log");

    ~MatchingEngine();

    auto start() -> void;
    auto stop()  -> void;

    MatchingEngine() = delete;
    MatchingEngine(const MatchingEngine &)             = delete;
    MatchingEngine(const MatchingEngine &&)            = delete;
    MatchingEngine &operator=(const MatchingEngine &)  = delete;
    MatchingEngine &operator=(const MatchingEngine &&) = delete;

    // Called by MEOrderBook on the hot path.
    // Placement-new into the LFQueue slot -- no heap, no copy through a temp.
    auto sendMarketUpdate(const MEMarketUpdate *market_update) noexcept -> void {
        auto *slot = market_updates_->getNextToWriteTo();
        new (slot) MEMarketUpdate(*market_update);
        market_updates_->updateWriteIndex();
        TTT_MEASURE(T4_MatchingEngine_LFQueue_write, logger_);
    }

    auto sendClientResponse(const MEClientResponse *client_response) noexcept -> void {
        auto *slot = outgoing_responses_->getNextToWriteTo();
        new (slot) MEClientResponse(*client_response);
        outgoing_responses_->updateWriteIndex();
        TTT_MEASURE(T4t_MatchingEngine_LFQueue_write, logger_);
    }

private:
    /// Reject a malformed request: tell the client (INVALID response) and log.
    /// Wire input must never index arrays unchecked or FATAL the engine.
    auto rejectClientRequest(const MEClientRequest *client_request,
                             const char *reason) noexcept -> void {
        const MEClientResponse reject{ClientResponseType::INVALID,
                                      client_request->client_id_,
                                      client_request->ticker_id_,
                                      client_request->order_id_, OrderId{},
                                      client_request->side_,
                                      client_request->price_,
                                      Qty{0}, Qty{0}};
        sendClientResponse(&reject);
        logger_.log("%:% %() % REJECTED (%) %\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    reason, client_request->toString());
    }

    auto processClientRequest(const MEClientRequest *client_request) noexcept -> void {
        const auto tid = client_request->ticker_id_.value;

        // ticker_id comes off the wire — bounds-check before it indexes
        // ticker_order_books_ (out-of-bounds write of a book pointer
        // otherwise).
        if (UNLIKELY(tid >= ticker_order_books_.size())) {
            rejectClientRequest(client_request, "invalid ticker");
            return;
        }

        // Lazy creation: only allocate order book when first referenced.
        if (UNLIKELY(!ticker_order_books_[tid])) {
            ticker_order_books_[tid] = new MEOrderBook(
                client_request->ticker_id_, &logger_, this);
        }

        auto *order_book = ticker_order_books_[tid];
        switch (client_request->type_) {
            case ClientRequestType::NEW: {
                START_MEASURE(Exchange_MEOrderBook_add);
                order_book->add(client_request->client_id_,
                                client_request->order_id_,
                                client_request->ticker_id_,
                                client_request->side_,
                                client_request->price_,
                                client_request->qty_);
                END_MEASURE(Exchange_MEOrderBook_add, logger_);
            } break;

            case ClientRequestType::CANCEL: {
                START_MEASURE(Exchange_MEOrderBook_cancel);
                order_book->cancel(client_request->client_id_,
                                   client_request->order_id_,
                                   client_request->ticker_id_);
                END_MEASURE(Exchange_MEOrderBook_cancel, logger_);
            } break;

            default:
                // Was FATAL() — a malformed type must not kill the exchange.
                // (OrderServer already filters non-NEW/CANCEL off the wire;
                // this is defence in depth for queue-level producers.)
                rejectClientRequest(client_request, "invalid request type");
                break;
        }
    }

    // Busy-spin loop. Exits only on the SHUTDOWN poison pill -- do NOT check
    // run_ on every iteration; that would serialise the hot path through an
    // atomic load.
    auto run() noexcept -> void {
        logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_));
        while (true) {
            const auto *req = incoming_requests_->getNextToRead();
            // UNLIKELY: queue is empty almost all the time.
            if (UNLIKELY(req)) {
                if (UNLIKELY(req->type_ == ClientRequestType::SHUTDOWN)) {
                    incoming_requests_->updateReadIndex();
                    break;
                }
                TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);
                logger_.log("%:% %() % Processing %\n",
                            __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str_),
                            req->toString());
                START_MEASURE(Exchange_MatchingEngine_processClientRequest);
                processClientRequest(req);
                END_MEASURE(Exchange_MatchingEngine_processClientRequest, logger_);
                incoming_requests_->updateReadIndex();
            }
        }
    }

    OrderBookHashMap      ticker_order_books_{};
    ClientRequestLFQueue  *incoming_requests_   = nullptr;
    ClientResponseLFQueue *outgoing_responses_  = nullptr;
    MEMarketUpdateLFQueue *market_updates_      = nullptr;

    std::atomic<bool> running_ = {false};  // NOT volatile

    std::string time_str_;
    Logger      logger_;
};

} // namespace Exchange
