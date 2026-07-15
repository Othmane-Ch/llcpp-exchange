#pragma once

#include <cstdint>
#include <cstdio>
#include "types.h"
#include "lq_free.h"

using namespace Common;

namespace Exchange {
#pragma pack(push, 1)
    enum class ClientResponseType : uint8_t {
        INVALID = 0,
        ACCEPTED = 1,
        CANCELED = 2,
        FILLED = 3,
        CANCEL_REJECTED = 4
    };

    inline std::string clientResponseTypeToString(ClientResponseType type) {
        switch (type) {
            case ClientResponseType::ACCEPTED:
                return "ACCEPTED";
            case ClientResponseType::CANCELED:
                return "CANCELED";
            case ClientResponseType::FILLED:
                return "FILLED";
            case ClientResponseType::CANCEL_REJECTED:
                return "CANCEL_REJECTED";
            case ClientResponseType::INVALID:
                return "INVALID";
        }
        return "UNKNOWN";
    }

    struct MEClientResponse {
        ClientResponseType type_ = ClientResponseType::INVALID;

        ClientId client_id_;
        TickerId ticker_id_;
        OrderId client_order_id_;
        OrderId market_order_id_;
        Side side_ = Side::INVALID;
        Price price_;
        Qty exec_qty_;
        Qty leaves_qty_;

        auto toString() const -> std::string {
            // Stack buffer + snprintf — same rationale as MEClientRequest.
            char buf[320];
            snprintf(buf, sizeof(buf),
                     "MEClientResponse [type:%s client:%s ticker:%s coid:%s moid:%s side:%s exec_qty:%s leaves_qty:%s price:%s]",
                     clientResponseTypeToString(type_).c_str(),
                     ClientId::toString(client_id_).c_str(),
                     TickerId::toString(ticker_id_).c_str(),
                     OrderId::toString(client_order_id_).c_str(),
                     OrderId::toString(market_order_id_).c_str(),
                     sideToString(side_).c_str(),
                     Qty::toString(exec_qty_).c_str(),
                     Qty::toString(leaves_qty_).c_str(),
                     Price::toString(price_).c_str());
            return buf;
        }
    };

    /// Public wire-format wrapper for client order responses.
    /// Adds a sequence number for application-level session synchronization over TCP.
    struct OMClientResponse {
        size_t seq_num_ = 0;
        MEClientResponse me_client_response_;

        auto toString() const -> std::string {
            char buf[384];
            snprintf(buf, sizeof(buf), "OMClientResponse [seq:%zu %s]",
                     seq_num_, me_client_response_.toString().c_str());
            return buf;
        }
    };

#pragma pack(pop)

    typedef LFQueue<MEClientResponse> ClientResponseLFQueue;
}
