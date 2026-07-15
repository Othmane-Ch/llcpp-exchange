// trading/trading_main.cpp
//
// Trading client entry point. Wires together:
//   - MarketDataConsumer  (subscribes to UDP multicast incremental + snapshot)
//   - TradeEngine         (busy-spins on inbound queues, owns sub-components)
//   - OrderGateway        (TCP client to the exchange's OrderServer)
//   - One of: MarketMaker / LiquidityTaker / random-order driver (smoke test)
//
// CLI:
//   trading_main CLIENT_ID ALGO_TYPE [CLIP THRESH MAX_ORDER_SIZE MAX_POS MAX_LOSS] ...
//
// One 5-tuple per ticker (ME_MAX_TICKERS max). Excess args are ignored;
// missing tickers default to a small clip / loose risk.
//
// Startup order matters:
//   1. Allocate LFQueues
//   2. Construct + start TradeEngine    (owns book + sub-components, drains queues)
//   3. Construct + start OrderGateway   (TCP, producer of ClientResponses)
//   4. Construct + start MarketDataConsumer (UDP, producer of MEMarketUpdates)
//   5. Sleep briefly to let threads reach their busy-spin loops
//   6. Instantiate the strategy plug-in (MM / LT) -- this overrides the
//      TradeEngine's default callbacks.
//
// Shutdown order matters (drain BEFORE stopping producers):
//   1. trade_engine->stop()
//   2. market_data_consumer->stop()
//   3. order_gateway->stop()
//   4. delete everything
//

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "types.h"
#include "logging.h"
#include "time_utils.h"

#include "client_request.h"
#include "client_response.h"
#include "market_update.h"

#include "trading/strategy/trade_engine.h"
#include "trading/strategy/market_maker.h"
#include "trading/strategy/liquidity_taker.h"
#include "trading/strategy/algo_type.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"

using namespace Common;

// ── Global pointers for signal-handler-driven shutdown ─────────────────────

Common::Logger              *logger                = nullptr;
Trading::TradeEngine        *trade_engine          = nullptr;
Trading::MarketDataConsumer *market_data_consumer  = nullptr;
Trading::OrderGateway       *order_gateway         = nullptr;
Trading::MarketMaker        *mm_algo               = nullptr;
Trading::LiquidityTaker     *taker_algo            = nullptr;

std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    g_shutdown.store(true);
}

// ── Argument parsing ───────────────────────────────────────────────────────

static auto parseTickerCfgs(int argc, char **argv) -> TradeEngineCfgHashMap {
    TradeEngineCfgHashMap cfg{};

    // Default loose risk so unconfigured tickers are still tradable in tests.
    for (size_t i = 0; i < cfg.size(); ++i) {
        cfg[i].clip_      = Qty{1};
        cfg[i].threshold_ = 0.0;
        cfg[i].risk_cfg_.max_order_size_ = Qty{100};
        cfg[i].risk_cfg_.max_position_   = Qty{1000};
        cfg[i].risk_cfg_.max_loss_       = -10000.0;
    }

    // Beyond program name + client_id + algo_type, args come in 5-tuples,
    // one tuple per ticker.
    constexpr int FIELDS_PER_TICKER = 5;
    int idx = 3;
    for (size_t t = 0; t < cfg.size() && (idx + FIELDS_PER_TICKER) <= argc; ++t) {
        cfg[t].clip_                     = Qty{static_cast<uint32_t>(std::atoi(argv[idx + 0]))};
        cfg[t].threshold_                = std::atof(argv[idx + 1]);
        cfg[t].risk_cfg_.max_order_size_ = Qty{static_cast<uint32_t>(std::atoi(argv[idx + 2]))};
        cfg[t].risk_cfg_.max_position_   = Qty{static_cast<uint32_t>(std::atoi(argv[idx + 3]))};
        cfg[t].risk_cfg_.max_loss_       = std::atof(argv[idx + 4]);
        idx += FIELDS_PER_TICKER;
    }

    return cfg;
}

// ── Random-mode order driver (smoke test only) ─────────────────────────────
//
// Used when the CLI selects AlgoType::RANDOM. Sends a small batch of NEW
// orders followed by CANCELs against a random previous order. Not on any
// hot path -- the sleeps are deliberate.
//
static auto runRandomDriver(Common::ClientId           client_id,
                            Trading::TradeEngine      *engine,
                            const TradeEngineCfgHashMap &cfg_map) -> void {
    constexpr int kIterations = 1'000;
    // Strong types aren't literal types (have non-trivial methods), so we
    // work in raw uint64_t and wrap when constructing requests. The base
    // price is derived from the ticker index (100, 200, ...) so it is valid
    // for any ME_MAX_TICKERS capacity profile.
    constexpr auto tickerBasePrice = [](uint32_t ticker) -> uint64_t {
        return 100 * (static_cast<uint64_t>(ticker) + 1);
    };

    std::vector<Exchange::MEClientRequest> sent;
    sent.reserve(kIterations);

    OrderId next_oid{1};
    for (int i = 0; i < kIterations && !g_shutdown.load(); ++i) {
        const TickerId t{static_cast<uint32_t>(rand() % ME_MAX_TICKERS)};
        const Side s = (rand() & 1) ? Side::BUY : Side::SELL;
        const uint64_t base = tickerBasePrice(t.value);
        const Price p{base + static_cast<uint64_t>(rand() % 5)};
        const Qty q{static_cast<uint32_t>(1 + (rand() % cfg_map[t.value].risk_cfg_.max_order_size_.value))};

        Exchange::MEClientRequest req{};
        req.type_      = Exchange::ClientRequestType::NEW;
        req.client_id_ = client_id;
        req.ticker_id_ = t;
        req.order_id_  = next_oid;
        ++next_oid;
        req.side_      = s;
        req.price_     = p;
        req.qty_       = q;

        engine->sendClientRequest(&req);
        sent.push_back(req);
        usleep(20'000);  // 20ms between orders

        // Occasionally cancel a prior order.
        if (!sent.empty() && (rand() % 4 == 0)) {
            auto cancel = sent[rand() % sent.size()];
            cancel.type_ = Exchange::ClientRequestType::CANCEL;
            engine->sendClientRequest(&cancel);
        }
    }
}

// ── main ───────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: %s CLIENT_ID ALGO_TYPE "
            "[CLIP THRESH MAX_ORDER_SIZE MAX_POS MAX_LOSS] ...\n"
            "  ALGO_TYPE = RANDOM | MAKER | TAKER\n",
            argv[0]);
        return EXIT_FAILURE;
    }

    const ClientId client_id{static_cast<uint32_t>(std::atoi(argv[1]))};
    const Trading::AlgoType algo_type = Trading::stringToAlgoType(argv[2]);
    if (algo_type == Trading::AlgoType::INVALID) {
        std::fprintf(stderr, "Unknown ALGO_TYPE: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    std::srand(client_id.value);

    logger = new Common::Logger("trading_main_" + std::to_string(client_id.value) + ".log");
    std::string time_str;

    std::signal(SIGINT, signal_handler);

    // ── Thread-affinity layout (recommended) ─────────────────────────────
    //
    // Production: client-side critical threads pin to dedicated cores on
    // a host with `isolcpus=3-5 nohz_full=3-5 rcu_nocbs=3-5` boot params:
    //   core 3 -- MarketDataConsumer
    //   core 4 -- OrderGateway
    //   core 5 -- TradeEngine
    //
    // On a smaller / shared host (e.g. running both exchange and client on
    // a 6-core VM as we do for benchmarking), pin counts collide and we
    // leave pinning off. The pinning hooks would have to be added to each
    // component's start() method to be effective; doing it from main()
    // alone is not possible since the component owns its pthread.
    //
    const long n_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_cores < 6) {
        std::cerr << "WARNING: host has " << n_cores
                  << " cores; recommended affinity layout needs 6. "
                     "Running without thread pinning. Latency tails will be "
                     "noisier than production." << std::endl;
    }

    const auto ticker_cfg = parseTickerCfgs(argc, argv);

    // ── LFQueues (allocated once at startup) ──────────────────────────────
    Exchange::ClientRequestLFQueue  client_requests (ME_MAX_CLIENT_UPDATES);
    Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
    Exchange::MEMarketUpdateLFQueue market_updates  (ME_MAX_MARKET_UPDATES);

    logger->log("%:% %() % Starting trading client client_id:% algo:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                getCurrentTimeStr(&time_str),
                client_id.value, Trading::algoTypeToString(algo_type));

    // ── 1) TradeEngine ────────────────────────────────────────────────────
    // Drains both inbound queues; must be running before any producer fills them.
    trade_engine = new Trading::TradeEngine(
        client_id, &client_requests, &client_responses, &market_updates,
        ticker_cfg, "trade_engine_" + std::to_string(client_id.value) + ".log");
    trade_engine->start();

    // ── 2) OrderGateway (TCP to the exchange's OrderServer) ───────────────
    const std::string order_iface = "lo";
    const std::string order_ip    = "127.0.0.1";
    const int         order_port  = 12345;
    order_gateway = new Trading::OrderGateway(
        client_id, &client_requests, &client_responses,
        order_ip, order_iface, order_port,
        "order_gateway_" + std::to_string(client_id.value) + ".log");
    order_gateway->start();

    // ── 3) MarketDataConsumer (UDP multicast: incremental + snapshot) ────
    const std::string md_iface          = "lo";
    const std::string md_snapshot_ip    = "239.0.0.1";
    const int         md_snapshot_port  = 20000;
    const std::string md_incremental_ip = "239.0.0.2";
    const int         md_incremental_port = 20001;
    market_data_consumer = new Trading::MarketDataConsumer(
        &market_updates, md_iface,
        md_snapshot_ip, md_snapshot_port,
        md_incremental_ip, md_incremental_port,
        "market_data_consumer_" + std::to_string(client_id.value) + ".log");
    market_data_consumer->start();

    // Let all threads reach their busy-spin loops before plugging in the
    // strategy. createAndStartThread no longer sleeps 1s per thread, so 2s
    // is ample on loopback; in production this would be an explicit
    // ready-state handshake.
    usleep(2 * 1000 * 1000);

    // ── 4) Strategy plug-in ───────────────────────────────────────────────
    //
    // TradeEngine already holds the FeatureEngine / OrderManager / etc.
    // (constructed in its production ctor). The strategy is wired in via the
    // engine's setBookUpdateCallback / setTradeUpdateCallback /
    // setOrderUpdateCallback (no need for the engine to own the strategy --
    // we own it here).
    //
    switch (algo_type) {
        case Trading::AlgoType::MAKER:
            mm_algo = new Trading::MarketMaker(
                logger, trade_engine,
                trade_engine->featureEngine(),
                trade_engine->orderManager(),
                ticker_cfg);
            break;

        case Trading::AlgoType::TAKER:
            taker_algo = new Trading::LiquidityTaker(
                logger, trade_engine,
                trade_engine->featureEngine(),
                trade_engine->orderManager(),
                ticker_cfg);
            break;

        case Trading::AlgoType::RANDOM:
            runRandomDriver(client_id, trade_engine, ticker_cfg);
            break;

        case Trading::AlgoType::INVALID:
        case Trading::AlgoType::MAX:
            // Already filtered by stringToAlgoType above.
            break;
    }

    logger->log("%:% %() % All components started.\n",
                __FILE__, __LINE__, __FUNCTION__,
                getCurrentTimeStr(&time_str));

    // ── Idle loop: wait for SIGINT ───────────────────────────────────────
    while (!g_shutdown.load()) {
        using namespace std::literals::chrono_literals;
        std::this_thread::sleep_for(1s);
    }

    logger->log("%:% %() % Shutdown signal received.\n",
                __FILE__, __LINE__, __FUNCTION__,
                getCurrentTimeStr(&time_str));

    // ── Shutdown: drain TradeEngine first, then stop producers ───────────
    if (trade_engine) {
        // Drain inbound queues before pulling the rug.
        while (client_responses.size() || market_updates.size()) {
            using namespace std::literals::chrono_literals;
            std::this_thread::sleep_for(10ms);
        }
        trade_engine->stop();
    }
    if (market_data_consumer) market_data_consumer->stop();
    if (order_gateway)        order_gateway->stop();

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);

    delete mm_algo;              mm_algo              = nullptr;
    delete taker_algo;           taker_algo           = nullptr;
    delete trade_engine;         trade_engine         = nullptr;
    delete market_data_consumer; market_data_consumer = nullptr;
    delete order_gateway;        order_gateway        = nullptr;
    delete logger;               logger               = nullptr;

    return EXIT_SUCCESS;
}
