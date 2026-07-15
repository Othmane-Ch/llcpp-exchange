#pragma once

#include <cstdint>
#include <cstdio>
#include "types.h"
#include "lq_free.h"

using namespace Common;

namespace Exchange {
#pragma pack(push, 1)
    enum class ClientRequestType : uint8_t {
        INVALID  = 0,
        NEW      = 1,
        CANCEL   = 2,
        SHUTDOWN = 3   // poison-pill: signals the matching engine run() loop to exit
    };

    inline std::string clientRequestTypeToString(ClientRequestType type) {
        switch (type) {
            case ClientRequestType::NEW:      return "NEW";
            case ClientRequestType::CANCEL:   return "CANCEL";
            case ClientRequestType::SHUTDOWN: return "SHUTDOWN";
            case ClientRequestType::INVALID:  return "INVALID";
        }
        return "UNKNOWN";
    }

    struct MEClientRequest {
        ClientRequestType type_ = ClientRequestType::INVALID;

        ClientId client_id_;
        TickerId ticker_id_;
        OrderId order_id_;
        Side side_ = Side::INVALID;
        Price price_;
        Qty qty_;

        auto toString() const -> std::string {
            // Stack buffer + snprintf: this runs on the matching-engine
            // thread per logged request; std::stringstream heap-allocates.
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "MEClientRequest [type:%s client:%s ticker:%s oid:%s side:%s qty:%s price:%s]",
                     clientRequestTypeToString(type_).c_str(),
                     ClientId::toString(client_id_).c_str(),
                     TickerId::toString(ticker_id_).c_str(),
                     OrderId::toString(order_id_).c_str(),
                     sideToString(side_).c_str(),
                     Qty::toString(qty_).c_str(),
                     Price::toString(price_).c_str());
            return buf;
        }
    };

    /// Public wire-format wrapper for client order requests.
    /// Adds a sequence number for application-level session synchronization over TCP.
    struct OMClientRequest {
        size_t seq_num_ = 0;
        MEClientRequest me_client_request_;

        auto toString() const -> std::string {
            char buf[320];
            snprintf(buf, sizeof(buf), "OMClientRequest [seq:%zu %s]",
                     seq_num_, me_client_request_.toString().c_str());
            return buf;
        }
    };

#pragma pack(pop)

    typedef LFQueue<MEClientRequest> ClientRequestLFQueue;
}
