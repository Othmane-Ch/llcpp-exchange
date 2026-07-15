// trading/strategy/market_order_book.cpp

#include "market_order_book.h"

#include "time_utils.h"
#include "perf_utils.h"

namespace Trading {

using namespace Common;
using Exchange::MEMarketUpdate;
using Exchange::MarketUpdateType;

// ── Constructor / Destructor ─────────────────────────────────────────────────

MarketOrderBook::MarketOrderBook(TickerId ticker_id, Logger *logger) noexcept
    : ticker_id_(ticker_id),
      logger_(logger),
      orders_at_price_pool_(ME_MAX_PRICE_LEVELS),
      order_pool_(ME_MAX_ORDER_IDS) {
    price_orders_at_price_.fill(nullptr);
    oid_to_order_.fill(nullptr);
}

MarketOrderBook::~MarketOrderBook() {
    if (logger_) {
        logger_->log("%:% %() % MarketOrderBook ticker:% destroyed\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_),
                     ticker_id_.value);
    }
    clearBook();
    bids_by_price_ = asks_by_price_ = nullptr;
}

// ── Level-chain maintenance (mirrors Exchange::MEOrderBook) ──────────────────

auto MarketOrderBook::addOrdersAtPrice(MarketOrdersAtPrice *new_level) noexcept -> void {
    price_orders_at_price_[priceToIndex(new_level->price_)] = new_level;

    auto &head_ref = (new_level->side_ == Side::BUY ? bids_by_price_ : asks_by_price_);

    if (UNLIKELY(!head_ref)) {
        // Empty side -- circular self-reference.
        head_ref = new_level;
        new_level->prev_entry_ = new_level;
        new_level->next_entry_ = new_level;
        return;
    }

    // Walk while new_level is LESS aggressive than target.
    //   BUY  (descending): new < target
    //   SELL (ascending):  new > target
    auto shouldAdvance = [&](MarketOrdersAtPrice *t) noexcept -> bool {
        return (new_level->side_ == Side::SELL && new_level->price_ > t->price_) ||
               (new_level->side_ == Side::BUY  && new_level->price_ < t->price_);
    };

    auto *target = head_ref;
    while (shouldAdvance(target) && target->next_entry_ != head_ref) {
        target = target->next_entry_;
    }

    if (shouldAdvance(target)) {
        // Less aggressive than everything -- append after target.
        auto *next = target->next_entry_; // wraps back to head_ref
        target->next_entry_ = new_level;
        new_level->prev_entry_ = target;
        new_level->next_entry_ = next;
        next->prev_entry_ = new_level;
    } else {
        // Insert BEFORE target.
        auto *prev = target->prev_entry_;
        prev->next_entry_ = new_level;
        new_level->prev_entry_ = prev;
        new_level->next_entry_ = target;
        target->prev_entry_ = new_level;
        if (head_ref == target) head_ref = new_level;
    }
}

auto MarketOrderBook::removeOrdersAtPrice(Side side, Price price) noexcept -> void {
    auto &head_ref = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
    auto *oap = price_orders_at_price_[priceToIndex(price)];
    if (UNLIKELY(!oap)) return;

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

    price_orders_at_price_[priceToIndex(price)] = nullptr;
    orders_at_price_pool_.deallocate(oap);
}

// ── Order-node maintenance ───────────────────────────────────────────────────

auto MarketOrderBook::addOrder(MarketOrder *order) noexcept -> void {
    auto *oap = price_orders_at_price_[priceToIndex(order->price_)];

    if (!oap) {
        // No level yet -- order forms a circular list of one.
        order->prev_order_ = order;
        order->next_order_ = order;
        auto *new_oap = orders_at_price_pool_.allocate(
            order->side_, order->price_, order,
            static_cast<MarketOrdersAtPrice *>(nullptr),
            static_cast<MarketOrdersAtPrice *>(nullptr));
        addOrdersAtPrice(new_oap);
    } else {
        // Append at the tail (prev of head is tail -- FIFO within price).
        auto *first = oap->first_mkt_order_;
        auto *last  = first->prev_order_;
        last->next_order_  = order;
        order->prev_order_ = last;
        order->next_order_ = first;
        first->prev_order_ = order;
    }

    oid_to_order_[order->order_id_.value] = order;
}

auto MarketOrderBook::removeOrder(MarketOrder *order) noexcept -> void {
    auto *prev = order->prev_order_;
    auto *next = order->next_order_;

    auto *oap = price_orders_at_price_[priceToIndex(order->price_)];

    if (prev == order) {
        // Only order at this level.
        removeOrdersAtPrice(order->side_, order->price_);
    } else {
        prev->next_order_ = next;
        next->prev_order_ = prev;
        if (oap && oap->first_mkt_order_ == order) oap->first_mkt_order_ = next;
    }

    // Poison pointers before pool reclaim -- prevents accidental reuse.
    order->prev_order_ = nullptr;
    order->next_order_ = nullptr;

    oid_to_order_[order->order_id_.value] = nullptr;
    order_pool_.deallocate(order);
}

// ── BBO refresh ──────────────────────────────────────────────────────────────

auto MarketOrderBook::updateBBO() noexcept -> void {
    // Sum every resting order at the top level -- cheap (typically 1-10 orders).
    if (bids_by_price_) {
        bbo_.bid_price_ = bids_by_price_->price_;
        uint32_t total = 0;
        auto *cur = bids_by_price_->first_mkt_order_;
        if (cur) {
            auto *it = cur;
            do { total += it->qty_.value; it = it->next_order_; } while (it != cur);
        }
        bbo_.bid_qty_ = Qty{total};
    } else {
        bbo_.bid_price_ = Price{};
        bbo_.bid_qty_   = Qty{0};
    }

    if (asks_by_price_) {
        bbo_.ask_price_ = asks_by_price_->price_;
        uint32_t total = 0;
        auto *cur = asks_by_price_->first_mkt_order_;
        if (cur) {
            auto *it = cur;
            do { total += it->qty_.value; it = it->next_order_; } while (it != cur);
        }
        bbo_.ask_qty_ = Qty{total};
    } else {
        bbo_.ask_price_ = Price{};
        bbo_.ask_qty_   = Qty{0};
    }
}

// ── CLEAR / snapshot wipe ────────────────────────────────────────────────────

auto MarketOrderBook::clearBook() noexcept -> void {
    // Walk every order slot; deallocate any live ones back to the pools.
    for (size_t i = 0; i < oid_to_order_.size(); ++i) {
        if (auto *o = oid_to_order_[i]) {
            o->prev_order_ = nullptr;
            o->next_order_ = nullptr;
            order_pool_.deallocate(o);
            oid_to_order_[i] = nullptr;
        }
    }
    for (size_t i = 0; i < price_orders_at_price_.size(); ++i) {
        if (auto *lvl = price_orders_at_price_[i]) {
            lvl->prev_entry_ = nullptr;
            lvl->next_entry_ = nullptr;
            orders_at_price_pool_.deallocate(lvl);
            price_orders_at_price_[i] = nullptr;
        }
    }
    bids_by_price_ = nullptr;
    asks_by_price_ = nullptr;
    bbo_ = BBO{};
}

// ── Public dispatch: one update in, state mutated ────────────────────────────

auto MarketOrderBook::onMarketUpdate(const MEMarketUpdate *u) noexcept -> void {
    switch (u->type_) {
        case MarketUpdateType::ADD: {
            // ADD: new resting order on the exchange book.
            // Bounds-check like MODIFY/CANCEL below: market order ids grow
            // monotonically, so a long session can exceed the direct-indexed
            // array — drop rather than write out of bounds.
            if (UNLIKELY(u->order_id_.value >= oid_to_order_.size())) {
                if (logger_) {
                    logger_->log("%:% %() % ADD order_id:% out of range — dropping\n",
                                 __FILE__, __LINE__, __FUNCTION__,
                                 Common::getCurrentTimeStr(&time_str_),
                                 u->order_id_.value);
                }
                return;
            }
            // Defense in depth, mirroring the exchange-side rejection: price
            // indexes price_orders_at_price_ via priceToIndex, so a price
            // ME_MAX_PRICE_LEVELS apart from a live level aliases its slot.
            // A well-behaved exchange never emits such an ADD (it rejects
            // the order) — drop it rather than corrupt the local book.
            const auto *aliased_level = getOrdersAtPrice(u->price_);
            if (UNLIKELY(aliased_level != nullptr &&
                         aliased_level->price_ != u->price_)) {
                if (logger_) {
                    logger_->log("%:% %() % ADD price:% aliases live level % — dropping\n",
                                 __FILE__, __LINE__, __FUNCTION__,
                                 Common::getCurrentTimeStr(&time_str_),
                                 Price::toString(u->price_),
                                 Price::toString(aliased_level->price_));
                }
                return;
            }
            auto *order = order_pool_.allocate(
                u->order_id_, u->side_, u->price_, u->qty_, u->priority_,
                static_cast<MarketOrder *>(nullptr),
                static_cast<MarketOrder *>(nullptr));
            START_MEASURE(Trading_MarketOrderBook_addOrder);
            addOrder(order);
            END_MEASURE(Trading_MarketOrderBook_addOrder, (*logger_));
        } break;

        case MarketUpdateType::MODIFY: {
            // MODIFY: existing order's remaining qty changed (partial fill).
            if (LIKELY(u->order_id_.value < oid_to_order_.size())) {
                if (auto *order = oid_to_order_[u->order_id_.value]) {
                    order->qty_ = u->qty_;
                }
            }
        } break;

        case MarketUpdateType::CANCEL: {
            // CANCEL: order fully consumed OR explicitly canceled.
            if (LIKELY(u->order_id_.value < oid_to_order_.size())) {
                if (auto *order = oid_to_order_[u->order_id_.value]) {
                    START_MEASURE(Trading_MarketOrderBook_removeOrder);
                    removeOrder(order);
                    END_MEASURE(Trading_MarketOrderBook_removeOrder, (*logger_));
                }
            }
        } break;

        case MarketUpdateType::TRADE:
            // TRADE is informational -- the paired CANCEL/MODIFY on the
            // passive order handles the book mutation. No state change here.
            break;

        case MarketUpdateType::CLEAR:
        case MarketUpdateType::SNAPSHOT_START:
            // Client must wipe its local book. SNAPSHOT_START/END bracketing
            // is driven by the MarketDataConsumer; here we simply clear.
            clearBook();
            break;

        case MarketUpdateType::SNAPSHOT_END:
            // No-op on the book itself -- the consumer uses SNAPSHOT_END to
            // decide when to resume the incremental stream.
            break;

        case MarketUpdateType::INVALID:
        default:
            if (logger_) {
                logger_->log("%:% %() % INVALID update type:% order_id:%\n",
                             __FILE__, __LINE__, __FUNCTION__,
                             Common::getCurrentTimeStr(&time_str_),
                             static_cast<int>(u->type_),
                             u->order_id_.value);
            }
            return;
    }

    START_MEASURE(Trading_MarketOrderBook_updateBBO);
    updateBBO();
    END_MEASURE(Trading_MarketOrderBook_updateBBO, (*logger_));
}

// ── toString ─────────────────────────────────────────────────────────────────

auto MarketOrderBook::toString(bool detailed) const -> std::string {
    std::string out = "MarketOrderBook ticker:" + std::to_string(ticker_id_.value);
    out += " " + bbo_.toString() + "\n";
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

} // namespace Trading
