// unordered_map_me_order_book.cpp
//
// Implementation of UnorderedMapMEOrderBook. The only behavioural
// differences from me_order_book.cpp are:
//
//   - lookups go through std::unordered_map::find / operator[] /
//     erase instead of std::array::operator[]
//   - the destructor no longer needs to zero a fixed-size 2D array;
//     unordered_map empties itself
//
// All matching, FIFO/priority, market-data, and response emission
// behaviour is preserved byte-for-byte.
//

#include "unordered_map_me_order_book.h"
#include "matcher/matching_engine.h"
#include "time_utils.h"

#include <algorithm>

namespace Exchange {

UnorderedMapMEOrderBook::UnorderedMapMEOrderBook(TickerId ticker_id, Logger *logger,
                                                  MatchingEngine *matching_engine)
    : ticker_id_(ticker_id),
      matching_engine_(matching_engine),
      orders_at_price_pool_(ME_MAX_PRICE_LEVELS),
      order_pool_(ME_MAX_ORDER_IDS),
      logger_(logger) {
    // No std::array fill needed -- unordered_maps default-construct empty.
}

UnorderedMapMEOrderBook::~UnorderedMapMEOrderBook() {
    logger_->log("%:% %() % UnorderedMapMEOrderBook\n%\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 toString(false, true));
    matching_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
    cid_oid_to_order_.clear();
    price_orders_at_price_.clear();
}

auto UnorderedMapMEOrderBook::getNextPriority(Price price) noexcept -> Priority {
    const auto *oap = getOrdersAtPrice(price);
    if (!oap) return Priority{1};
    return Priority{oap->first_me_order_->prev_order_->priority_.value + 1};
}

auto UnorderedMapMEOrderBook::addOrdersAtPrice(MEOrdersAtPrice *new_level) noexcept -> void {
    // operator[] inserts if missing; here it's a fresh price, no rebalance issue
    // because the level pointer is the single value being stored.
    price_orders_at_price_[new_level->price_] = new_level;

    auto &head_ref = (new_level->side_ == Side::BUY ? bids_by_price_ : asks_by_price_);

    if (UNLIKELY(!head_ref)) {
        head_ref = new_level;
        new_level->prev_entry_ = new_level;
        new_level->next_entry_ = new_level;
        return;
    }

    auto shouldAdvance = [&](MEOrdersAtPrice *t) noexcept -> bool {
        return (new_level->side_ == Side::SELL && new_level->price_ > t->price_) ||
               (new_level->side_ == Side::BUY  && new_level->price_ < t->price_);
    };

    auto *target = head_ref;
    while (shouldAdvance(target) && target->next_entry_ != head_ref) {
        target = target->next_entry_;
    }

    if (shouldAdvance(target)) {
        auto *next = target->next_entry_;
        target->next_entry_ = new_level;
        new_level->prev_entry_ = target;
        new_level->next_entry_ = next;
        next->prev_entry_ = new_level;
    } else {
        auto *prev = target->prev_entry_;
        prev->next_entry_ = new_level;
        new_level->prev_entry_ = prev;
        new_level->next_entry_ = target;
        target->prev_entry_ = new_level;
        if (head_ref == target) head_ref = new_level;
    }
}

auto UnorderedMapMEOrderBook::removeOrdersAtPrice(Side side, Price price) noexcept -> void {
    auto &head_ref = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
    auto *oap = getOrdersAtPrice(price);

    if (UNLIKELY(oap->next_entry_ == oap)) {
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

    price_orders_at_price_.erase(price);
    orders_at_price_pool_.deallocate(oap);
}

auto UnorderedMapMEOrderBook::addOrder(MEOrder *order) noexcept -> void {
    auto *oap = getOrdersAtPrice(order->price_);

    if (!oap) {
        order->prev_order_ = order;
        order->next_order_ = order;
        auto *new_oap = orders_at_price_pool_.allocate(
            order->side_, order->price_, order,
            static_cast<MEOrdersAtPrice *>(nullptr),
            static_cast<MEOrdersAtPrice *>(nullptr));
        addOrdersAtPrice(new_oap);
    } else {
        auto *first = oap->first_me_order_;
        auto *last  = first->prev_order_;
        last->next_order_  = order;
        order->prev_order_ = last;
        order->next_order_ = first;
        first->prev_order_ = order;
    }

    cid_oid_to_order_[order->client_id_][order->client_order_id_] = order;
}

auto UnorderedMapMEOrderBook::removeOrder(MEOrder *order) noexcept -> void {
    auto *prev = order->prev_order_;
    auto *next = order->next_order_;
    order->prev_order_ = nullptr;
    order->next_order_ = nullptr;

    auto *oap = getOrdersAtPrice(order->price_);

    if (prev == order) {
        removeOrdersAtPrice(order->side_, order->price_);
    } else {
        prev->next_order_ = next;
        next->prev_order_ = prev;
        if (oap->first_me_order_ == order) oap->first_me_order_ = next;
    }

    auto client_it = cid_oid_to_order_.find(order->client_id_);
    if (client_it != cid_oid_to_order_.end()) {
        client_it->second.erase(order->client_order_id_);
    }
    order_pool_.deallocate(order);
}

auto UnorderedMapMEOrderBook::match(TickerId ticker_id, ClientId client_id, Side side,
                                     OrderId client_oid, OrderId new_moid,
                                     MEOrder *passive, Qty *leaves_qty) noexcept -> void {
    const auto order_qty = passive->qty_;
    const auto fill_qty  = Qty{std::min(leaves_qty->value, order_qty.value)};

    leaves_qty->value   -= fill_qty.value;
    passive->qty_.value -= fill_qty.value;

    client_response_ = {ClientResponseType::FILLED,
                        client_id, ticker_id,
                        client_oid, new_moid,
                        side, passive->price_,
                        fill_qty, *leaves_qty};
    matching_engine_->sendClientResponse(&client_response_);

    client_response_ = {ClientResponseType::FILLED,
                        passive->client_id_, ticker_id,
                        passive->client_order_id_, passive->market_order_id_,
                        passive->side_, passive->price_,
                        fill_qty, passive->qty_};
    matching_engine_->sendClientResponse(&client_response_);

    market_update_ = {MarketUpdateType::TRADE,
                      ticker_id, side,
                      passive->price_, fill_qty,
                      OrderId{}, Priority{}};
    matching_engine_->sendMarketUpdate(&market_update_);

    if (passive->qty_.value == 0) {
        market_update_ = {MarketUpdateType::CANCEL,
                          ticker_id, passive->side_,
                          passive->price_, order_qty,
                          passive->market_order_id_, passive->priority_};
        matching_engine_->sendMarketUpdate(&market_update_);
        removeOrder(passive);
    } else {
        market_update_ = {MarketUpdateType::MODIFY,
                          ticker_id, passive->side_,
                          passive->price_, passive->qty_,
                          passive->market_order_id_, passive->priority_};
        matching_engine_->sendMarketUpdate(&market_update_);
    }
}

auto UnorderedMapMEOrderBook::checkForMatch(ClientId client_id, OrderId client_oid,
                                             TickerId ticker_id, Side side,
                                             Price price, Qty qty,
                                             OrderId new_moid) noexcept -> Qty {
    auto leaves_qty = qty;

    if (side == Side::BUY) {
        while (leaves_qty.value > 0) {
            auto *best_ask = asks_by_price_;
            if (LIKELY(!best_ask || price < best_ask->price_)) break;
            match(ticker_id, client_id, side, client_oid, new_moid,
                  best_ask->first_me_order_, &leaves_qty);
        }
    } else {
        while (leaves_qty.value > 0) {
            auto *best_bid = bids_by_price_;
            if (LIKELY(!best_bid || price > best_bid->price_)) break;
            match(ticker_id, client_id, side, client_oid, new_moid,
                  best_bid->first_me_order_, &leaves_qty);
        }
    }

    return leaves_qty;
}

auto UnorderedMapMEOrderBook::add(ClientId client_id, OrderId client_order_id,
                                   TickerId ticker_id, Side side,
                                   Price price, Qty qty) noexcept -> void {
    latency_tracker_.start();
    const auto new_moid = generateNextMarketOrderId();

    client_response_ = {ClientResponseType::ACCEPTED,
                        client_id, ticker_id,
                        client_order_id, new_moid,
                        side, price,
                        Qty{0}, qty};
    matching_engine_->sendClientResponse(&client_response_);

    const auto leaves_qty = checkForMatch(
        client_id, client_order_id, ticker_id, side, price, qty, new_moid);

    if (LIKELY(leaves_qty.value > 0)) {
        const auto priority = getNextPriority(price);
        auto *order = order_pool_.allocate(
            ticker_id, client_id, client_order_id, new_moid,
            side, price, leaves_qty, priority,
            static_cast<MEOrder *>(nullptr),
            static_cast<MEOrder *>(nullptr));
        addOrder(order);

        market_update_ = {MarketUpdateType::ADD,
                          ticker_id, side,
                          price, leaves_qty,
                          new_moid, priority};
        matching_engine_->sendMarketUpdate(&market_update_);
    }
    latency_tracker_.stop();
}

auto UnorderedMapMEOrderBook::cancel(ClientId client_id, OrderId order_id,
                                      TickerId ticker_id) noexcept -> void {
    latency_tracker_.start();

    MEOrder *exchange_order = nullptr;
    auto client_it = cid_oid_to_order_.find(client_id);
    if (client_it != cid_oid_to_order_.end()) {
        auto order_it = client_it->second.find(order_id);
        if (order_it != client_it->second.end()) {
            exchange_order = order_it->second;
        }
    }

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

    removeOrder(exchange_order);

    matching_engine_->sendMarketUpdate(&market_update_);
    matching_engine_->sendClientResponse(&client_response_);
    latency_tracker_.stop();
}

auto UnorderedMapMEOrderBook::toString(bool detailed, bool /*validity_check*/) const -> std::string {
    std::string out = "UnorderedMapMEOrderBook ticker:" + std::to_string(ticker_id_.value) + "\n";
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
