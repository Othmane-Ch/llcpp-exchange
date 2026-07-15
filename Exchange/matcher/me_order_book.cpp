
#include "me_order_book.h"
#include "matcher/matching_engine.h"
#include "time_utils.h"

#include <algorithm>

namespace Exchange {

// -- Constructor / Destructor -------------------------------------------------

MEOrderBook::MEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine)
    : ticker_id_(ticker_id),
      matching_engine_(matching_engine),
      orders_at_price_pool_(ME_MAX_PRICE_LEVELS),
      order_pool_(ME_MAX_ORDER_IDS),
      logger_(logger) {
    for (auto &row : cid_oid_to_order_) row.fill(nullptr);
    price_orders_at_price_.fill(nullptr);
}

MEOrderBook::~MEOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 toString(false, true));
    matching_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
    for (auto &row : cid_oid_to_order_) row.fill(nullptr);
}

// -- Price-level helpers ------------------------------------------------------

auto MEOrderBook::getNextPriority(Price price) noexcept -> Priority {
    const auto *oap = getOrdersAtPrice(price);
    if (!oap) return Priority{1};
    // prev_order_ of the circular list's head IS the tail -- O(1) access.
    return Priority{oap->first_me_order_->prev_order_->priority_.value + 1};
}

auto MEOrderBook::addOrdersAtPrice(MEOrdersAtPrice *new_level) noexcept -> void {
    price_orders_at_price_[priceToIndex(new_level->price_)] = new_level;

    auto &head_ref = (new_level->side_ == Side::BUY ? bids_by_price_ : asks_by_price_);

    if (UNLIKELY(!head_ref)) {
        // Empty side -- circular self-reference.
        head_ref = new_level;
        new_level->prev_entry_ = new_level;
        new_level->next_entry_ = new_level;
        return;
    }

    // Walk forward while new_level is LESS aggressive than the current target.
    // BUY  (descending): less aggressive means lower price  -> advance while new < target
    // SELL (ascending):  less aggressive means higher price -> advance while new > target
    auto shouldAdvance = [&](MEOrdersAtPrice *t) noexcept -> bool {
        return (new_level->side_ == Side::SELL && new_level->price_ > t->price_) ||
               (new_level->side_ == Side::BUY  && new_level->price_ < t->price_);
    };

    auto *target = head_ref;
    while (shouldAdvance(target) && target->next_entry_ != head_ref) {
        target = target->next_entry_;
    }

    if (shouldAdvance(target)) {
        // New level is less aggressive than everything; insert AFTER target (append).
        auto *next = target->next_entry_; // wraps back to head_ref
        target->next_entry_ = new_level;
        new_level->prev_entry_ = target;
        new_level->next_entry_ = next;
        next->prev_entry_ = new_level;
    } else {
        // Insert BEFORE target; target is the first level new_level beats.
        auto *prev = target->prev_entry_;
        prev->next_entry_ = new_level;
        new_level->prev_entry_ = prev;
        new_level->next_entry_ = target;
        target->prev_entry_ = new_level;
        // If we are inserting before the current head, new_level is the new head.
        if (head_ref == target) head_ref = new_level;
    }
}

auto MEOrderBook::removeOrdersAtPrice(Side side, Price price) noexcept -> void {
    auto &head_ref = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
    auto *oap = getOrdersAtPrice(price);

    if (UNLIKELY(oap->next_entry_ == oap)) {
        // Last level on this side.
        head_ref = nullptr;
    } else {
        auto *prev = oap->prev_entry_;
        auto *next = oap->next_entry_;
        prev->next_entry_ = next;
        next->prev_entry_ = prev;
        if (head_ref == oap) head_ref = next;
        oap->prev_entry_ = nullptr;
        oap->next_entry_ = nullptr;
    }

    price_orders_at_price_[priceToIndex(price)] = nullptr;
    orders_at_price_pool_.deallocate(oap);
}

// -- Order-node helpers -------------------------------------------------------

auto MEOrderBook::addOrder(MEOrder *order) noexcept -> void {
    auto *oap = getOrdersAtPrice(order->price_);

    if (!oap) {
        // Case A: no level yet -- order forms a circular list of one.
        order->prev_order_ = order;
        order->next_order_ = order;
        auto *new_oap = orders_at_price_pool_.allocate(
            order->side_, order->price_, order,
            static_cast<MEOrdersAtPrice *>(nullptr),
            static_cast<MEOrdersAtPrice *>(nullptr));
        addOrdersAtPrice(new_oap);
    } else {
        // Case B: append at the TAIL (FIFO; prev of the head is the tail).
        auto *first = oap->first_me_order_;
        auto *last  = first->prev_order_;
        last->next_order_  = order;
        order->prev_order_ = last;
        order->next_order_ = first;
        first->prev_order_ = order;
    }

    cid_oid_to_order_[order->client_id_.value][order->client_order_id_.value] = order;
}

auto MEOrderBook::removeOrder(MEOrder *order) noexcept -> void {
    auto *prev = order->prev_order_;
    auto *next = order->next_order_;
    // Poison immediately -- prevents use-after-free in call chains.
    order->prev_order_ = nullptr;
    order->next_order_ = nullptr;

    auto *oap = getOrdersAtPrice(order->price_);

    if (prev == order) {
        // Only order at this level.
        removeOrdersAtPrice(order->side_, order->price_);
    } else {
        prev->next_order_ = next;
        next->prev_order_ = prev;
        if (oap->first_me_order_ == order) oap->first_me_order_ = next;
    }

    cid_oid_to_order_[order->client_id_.value][order->client_order_id_.value] = nullptr;
    order_pool_.deallocate(order);
}

// -- Matching logic -----------------------------------------------------------

auto MEOrderBook::match(TickerId ticker_id, ClientId client_id, Side side,
                        OrderId client_oid, OrderId new_moid,
                        MEOrder *passive, Qty *leaves_qty) noexcept -> void {
    const auto order_qty = passive->qty_;  // snapshot BEFORE any mutation
    const auto fill_qty  = Qty{std::min(leaves_qty->value, order_qty.value)};

    leaves_qty->value   -= fill_qty.value;
    passive->qty_.value -= fill_qty.value;

    // FILLED -- aggressive side
    client_response_ = {ClientResponseType::FILLED,
                        client_id, ticker_id,
                        client_oid, new_moid,
                        side, passive->price_,
                        fill_qty, *leaves_qty};
    matching_engine_->sendClientResponse(&client_response_);

    // FILLED -- passive side
    client_response_ = {ClientResponseType::FILLED,
                        passive->client_id_, ticker_id,
                        passive->client_order_id_, passive->market_order_id_,
                        passive->side_, passive->price_,
                        fill_qty, passive->qty_};
    matching_engine_->sendClientResponse(&client_response_);

    // TRADE market update (order_id and priority are invalid for trades)
    market_update_ = {MarketUpdateType::TRADE,
                      ticker_id, side,
                      passive->price_, fill_qty,
                      OrderId{}, Priority{}};
    matching_engine_->sendMarketUpdate(&market_update_);

    if (passive->qty_.value == 0) {
        // Fully consumed -- CANCEL update uses original qty, not 0.
        market_update_ = {MarketUpdateType::CANCEL,
                          ticker_id, passive->side_,
                          passive->price_, order_qty,
                          passive->market_order_id_, passive->priority_};
        matching_engine_->sendMarketUpdate(&market_update_);
        START_MEASURE(Exchange_MEOrderBook_removeOrder);
        removeOrder(passive);
        END_MEASURE(Exchange_MEOrderBook_removeOrder, (*logger_));
    } else {
        // Partially consumed -- MODIFY update with remaining qty.
        market_update_ = {MarketUpdateType::MODIFY,
                          ticker_id, passive->side_,
                          passive->price_, passive->qty_,
                          passive->market_order_id_, passive->priority_};
        matching_engine_->sendMarketUpdate(&market_update_);
    }
}

auto MEOrderBook::checkForMatch(ClientId client_id, OrderId client_oid,
                                TickerId ticker_id, Side side,
                                Price price, Qty qty,
                                OrderId new_moid) noexcept -> Qty {
    auto leaves_qty = qty;

    if (side == Side::BUY) {
        while (leaves_qty.value > 0) {
            auto *best_ask = asks_by_price_;
            // Most orders are passive -- break is the common case.
            if (LIKELY(!best_ask || price < best_ask->price_)) break;
            // Re-fetch first_me_order_ each iteration: match() may have removed the level.
            START_MEASURE(Exchange_MEOrderBook_match);
            match(ticker_id, client_id, side, client_oid, new_moid,
                  best_ask->first_me_order_, &leaves_qty);
            END_MEASURE(Exchange_MEOrderBook_match, (*logger_));
        }
    } else { // SELL
        while (leaves_qty.value > 0) {
            auto *best_bid = bids_by_price_;
            // Most orders are passive -- break is the common case.
            if (LIKELY(!best_bid || price > best_bid->price_)) break;
            START_MEASURE(Exchange_MEOrderBook_match);
            match(ticker_id, client_id, side, client_oid, new_moid,
                  best_bid->first_me_order_, &leaves_qty);
            END_MEASURE(Exchange_MEOrderBook_match, (*logger_));
        }
    }

    return leaves_qty;
}

// -- Public interface ---------------------------------------------------------

auto MEOrderBook::add(ClientId client_id, OrderId client_order_id,
                      TickerId ticker_id, Side side,
                      Price price, Qty qty) noexcept -> void {
    latency_tracker_.start();

    // client_id and client_order_id arrive off the wire and directly index
    // cid_oid_to_order_ — validate or a hostile/buggy client corrupts memory.
    // A duplicate live client_order_id would orphan the resting order (its
    // map slot gets overwritten, making it uncancelable) — reject instead.
    if (UNLIKELY(client_id.value >= cid_oid_to_order_.size() ||
                 client_order_id.value >= cid_oid_to_order_[0].size() ||
                 cid_oid_to_order_[client_id.value][client_order_id.value] != nullptr)) {
        client_response_ = {ClientResponseType::INVALID,
                            client_id, ticker_id,
                            client_order_id, OrderId{},
                            side, price,
                            Qty{0}, Qty{0}};
        matching_engine_->sendClientResponse(&client_response_);
        latency_tracker_.stop();
        return;
    }

    // price also arrives off the wire and directly indexes
    // price_orders_at_price_ via priceToIndex (price % ME_MAX_PRICE_LEVELS),
    // so two live prices ME_MAX_PRICE_LEVELS apart alias to one slot: resting
    // this order would append it to the WRONG price level, and removing
    // either level would clear the shared slot. Reject the aliasing order
    // (same pattern as the duplicate-order-id rejection above) to preserve
    // the invariant that a live slot always holds the level whose price maps
    // to it — that invariant is what keeps every other O(1) lookup
    // (cancel/removeOrder/getNextPriority) valid.
    const auto *aliased_level = getOrdersAtPrice(price);
    if (UNLIKELY(aliased_level != nullptr && aliased_level->price_ != price)) {
        client_response_ = {ClientResponseType::INVALID,
                            client_id, ticker_id,
                            client_order_id, OrderId{},
                            side, price,
                            Qty{0}, Qty{0}};
        matching_engine_->sendClientResponse(&client_response_);
        logger_->log("%:% %() % REJECTED price-collision: price % aliases live level %\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_),
                     Price::toString(price),
                     Price::toString(aliased_level->price_));
        latency_tracker_.stop();
        return;
    }

    const auto new_moid = generateNextMarketOrderId();

    // ACCEPTED: client is acknowledged before matching begins.
    client_response_ = {ClientResponseType::ACCEPTED,
                        client_id, ticker_id,
                        client_order_id, new_moid,
                        side, price,
                        Qty{0}, qty};
    matching_engine_->sendClientResponse(&client_response_);

    START_MEASURE(Exchange_MEOrderBook_checkForMatch);
    const auto leaves_qty = checkForMatch(
        client_id, client_order_id, ticker_id, side, price, qty, new_moid);
    END_MEASURE(Exchange_MEOrderBook_checkForMatch, (*logger_));

    // LIKELY: most orders are passive and will rest in the book.
    if (LIKELY(leaves_qty.value > 0)) {
        const auto priority = getNextPriority(price);
        auto *order = order_pool_.allocate(
            ticker_id, client_id, client_order_id, new_moid,
            side, price, leaves_qty, priority,
            static_cast<MEOrder *>(nullptr),
            static_cast<MEOrder *>(nullptr));
        START_MEASURE(Exchange_MEOrderBook_addOrder);
        addOrder(order);
        END_MEASURE(Exchange_MEOrderBook_addOrder, (*logger_));

        market_update_ = {MarketUpdateType::ADD,
                          ticker_id, side,
                          price, leaves_qty,
                          new_moid, priority};
        matching_engine_->sendMarketUpdate(&market_update_);
    }
    latency_tracker_.stop();
}

auto MEOrderBook::cancel(ClientId client_id, OrderId order_id,
                         TickerId ticker_id) noexcept -> void {
    latency_tracker_.start();
    // Validate -- bounds check then pointer lookup.
    auto *exchange_order =
        (client_id.value < cid_oid_to_order_.size() &&
         order_id.value  < cid_oid_to_order_[0].size())
        ? cid_oid_to_order_[client_id.value][order_id.value]
        : nullptr;

    if (UNLIKELY(!exchange_order)) {
        client_response_ = {ClientResponseType::CANCEL_REJECTED,
                            client_id, ticker_id,
                            order_id, OrderId{},
                            Side::INVALID, Price{},
                            Qty{0}, Qty{0}};
        matching_engine_->sendClientResponse(&client_response_);
        latency_tracker_.stop();
        return;
    }

    // Capture all fields BEFORE removeOrder -- pool reclaims the memory.
    client_response_ = {ClientResponseType::CANCELED,
                        client_id, ticker_id,
                        exchange_order->client_order_id_,
                        exchange_order->market_order_id_,
                        exchange_order->side_,
                        exchange_order->price_,
                        Qty{0}, Qty{0}};

    market_update_ = {MarketUpdateType::CANCEL,
                      ticker_id,
                      exchange_order->side_,
                      exchange_order->price_,
                      exchange_order->qty_,
                      exchange_order->market_order_id_,
                      exchange_order->priority_};

    START_MEASURE(Exchange_MEOrderBook_removeOrder);
    removeOrder(exchange_order);
    END_MEASURE(Exchange_MEOrderBook_removeOrder, (*logger_));

    matching_engine_->sendMarketUpdate(&market_update_);
    matching_engine_->sendClientResponse(&client_response_);
    latency_tracker_.stop();
}

auto MEOrderBook::toString(bool detailed, bool /*validity_check*/) const -> std::string {
    std::string out = "MEOrderBook ticker:" + std::to_string(ticker_id_.value) + "\n";
    if (!detailed) return out;

    out += "  BIDS:\n";
    if (bids_by_price_) {
        const auto *lvl = bids_by_price_;
        do {
            out += "    " + lvl->toString() + "\n";
            lvl = lvl->next_entry_;
        } while (lvl != bids_by_price_);
    }

    out += "  ASKS:\n";
    if (asks_by_price_) {
        const auto *lvl = asks_by_price_;
        do {
            out += "    " + lvl->toString() + "\n";
            lvl = lvl->next_entry_;
        } while (lvl != asks_by_price_);
    }

    return out;
}

} // namespace Exchange
