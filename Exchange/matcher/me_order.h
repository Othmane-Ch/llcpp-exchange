
#pragma once

#include <array>
#include "types.h"

using namespace Common;

namespace Exchange {

struct MEOrder {
    TickerId ticker_id_;
    ClientId client_id_;
    OrderId client_order_id_;
    OrderId market_order_id_;
    Side side_ = Side::INVALID;
    Price price_;
    Qty qty_;
    Priority priority_;

    MEOrder *prev_order_ = nullptr;
    MEOrder *next_order_ = nullptr;

    MEOrder() = default;

    MEOrder(TickerId ticker_id, ClientId client_id,
            OrderId client_order_id, OrderId market_order_id,
            Side side, Price price, Qty qty, Priority priority,
            MEOrder *prev_order, MEOrder *next_order) noexcept
        : ticker_id_(ticker_id), client_id_(client_id),
          client_order_id_(client_order_id), market_order_id_(market_order_id),
          side_(side), price_(price), qty_(qty),
          priority_(priority),
          prev_order_(prev_order),
          next_order_(next_order) {}

    auto toString() const -> std::string {
        return "MEOrder["
               "ticker:" + TickerId::toString(ticker_id_) +
               " client:" + ClientId::toString(client_id_) +
               " client_oid:" + OrderId::toString(client_order_id_) +
               " market_oid:" + OrderId::toString(market_order_id_) +
               " side:" + sideToString(side_) +
               " price:" + Price::toString(price_) +
               " qty:" + Qty::toString(qty_) +
               " priority:" + Priority::toString(priority_) +
               " prev:" + OrderId::toString(prev_order_ ? prev_order_->market_order_id_ : OrderId()) +
               " next:" + OrderId::toString(next_order_ ? next_order_->market_order_id_ : OrderId()) +
               "]";
    }
};

using OrderHashMap = std::array<MEOrder *, ME_MAX_ORDER_IDS>;
using ClientOrderHashMap = std::array<OrderHashMap, ME_MAX_NUM_CLIENTS>;

struct MEOrdersAtPrice {
    Side side_ = Side::INVALID;
    Price price_;

    MEOrder *first_me_order_ = nullptr;

    MEOrdersAtPrice *prev_entry_ = nullptr;
    MEOrdersAtPrice *next_entry_ = nullptr;

    MEOrdersAtPrice() = default;

    MEOrdersAtPrice(Side side, Price price, MEOrder *first_me_order,
                    MEOrdersAtPrice *prev_entry, MEOrdersAtPrice *next_entry) noexcept
        : side_(side), price_(price), first_me_order_(first_me_order),
          prev_entry_(prev_entry), next_entry_(next_entry) {}

    auto toString() const -> std::string {
        return "MEOrdersAtPrice["
               "side:" + sideToString(side_) +
               " price:" + Price::toString(price_) +
               " first_order:" + (first_me_order_ ? first_me_order_->toString() : "null") +
               " prev_entry:" + (prev_entry_ ? Price::toString(prev_entry_->price_) : "null") +
               " next_entry:" + (next_entry_ ? Price::toString(next_entry_->price_) : "null") +
               "]";
    }
};

using OrdersAtPriceHashMap = std::array<MEOrdersAtPrice *, ME_MAX_PRICE_LEVELS>;

} // namespace Exchange
