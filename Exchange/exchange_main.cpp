//
// exchange_main.cpp — ties all exchange components together.
//
// Thread model:
//   Thread 1: MatchingEngine        — busy-spin on incoming order queue
//   Thread 2: OrderServer           — busy-spin on TCP + response queue
//   Thread 3: MarketDataPublisher   — busy-spin on market update queue
//   Thread 4: SnapshotSynthesizer   — busy-spin on snapshot queue (inside publisher)
//
// The ONLY shared state between threads is the SPSC lock-free queues.
// No locks anywhere.
//

#include "matcher/matching_engine.h"
#include "order_server/order_server.h"
#include "market_data/market_data_publisher.h"
#include <atomic>
#include <csignal>
#include <iostream>
#include <unistd.h>

/// Global pointers for shutdown (cleanup runs on the main thread, not in the
/// signal handler).
Common::Logger           *logger            = nullptr;
Exchange::MatchingEngine *matching_engine    = nullptr;
Exchange::OrderServer    *order_server       = nullptr;
Exchange::MarketDataPublisher *market_data_publisher = nullptr;

/// Signal handlers may only touch async-signal-safe state. The previous
/// handler slept 20s and ran destructors (heap frees, joins, iostreams) from
/// signal context — undefined behaviour and a 20-second Ctrl+C. Set a flag;
/// main() observes it and tears down normally.
std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    g_shutdown.store(true);
}

int main(int, char **) {
    logger = new Common::Logger("exchange_main.log");
    std::string time_str;

    signal(SIGINT, signal_handler);

    // ── Thread-affinity layout (recommended) ─────────────────────────────
    //
    // Production layout (requires `isolcpus=0-5 nohz_full=0-5 rcu_nocbs=0-5`
    // boot parameters):
    //   core 0 -- MarketDataPublisher
    //   core 1 -- MatchingEngine
    //   core 2 -- OrderServer
    //   core 3 -- MarketDataConsumer  (client-side, on a separate host)
    //   core 4 -- OrderGateway        (client-side, on a separate host)
    //   core 5 -- TradeEngine         (client-side, on a separate host)
    //
    // On hosts with fewer cores than the layout requires (we need three
    // dedicated cores on the exchange side plus headroom for logger flush
    // threads + main idle), we leave pinning off and tolerate the latency
    // tail jitter. The pinning hooks would need to be added to each
    // component's start() method; doing it from main() alone is not
    // possible because the component owns its pthread.
    //
    const long n_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_cores < 6) {
        std::cerr << "WARNING: host has " << n_cores
                  << " cores; recommended affinity layout needs 6. "
                     "Running without thread pinning. Latency tails will be "
                     "noisier than production." << std::endl;
    }

    // ── Lock-free queues (allocated once at startup, shared via pointers) ──
    Exchange::ClientRequestLFQueue  client_requests(ME_MAX_CLIENT_UPDATES);
    Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
    Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

    logger->log("%:% %() % Starting Exchange...\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str));

    // ── Component 1: Matching Engine (Thread 1) ──
    matching_engine = new Exchange::MatchingEngine(
        &client_requests, &client_responses, &market_updates);
    matching_engine->start();

    // ── Component 2: Order Server (Thread 2) ──
    const std::string order_iface = "lo";
    const int         order_port  = 12345;
    order_server = new Exchange::OrderServer(
        &client_requests, &client_responses, order_iface, order_port);
    order_server->start();

    // ── Component 3: Market Data Publisher (Thread 3 + Thread 4 synthesizer) ──
    const std::string md_iface          = "lo";
    const std::string md_snapshot_ip    = "239.0.0.1";
    const int         md_snapshot_port  = 20000;
    const std::string md_incremental_ip = "239.0.0.2";
    const int         md_incremental_port = 20001;
    market_data_publisher = new Exchange::MarketDataPublisher(
        &market_updates, md_iface,
        md_snapshot_ip, md_snapshot_port,
        md_incremental_ip, md_incremental_port);
    market_data_publisher->start();

    logger->log("%:% %() % All components started. Ctrl+C to stop.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str));

    // Main thread idles — all real work happens on dedicated threads.
    // Poll the shutdown flag at 100ms so Ctrl+C tears down promptly.
    using namespace std::literals::chrono_literals;
    int idle_ticks = 0;
    while (!g_shutdown.load()) {
        if (idle_ticks++ % 300 == 0) { // ~every 30s
            logger->log("%:% %() % Exchange running...\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str));
        }
        std::this_thread::sleep_for(100ms);
    }

    logger->log("%:% %() % Shutdown signal received.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str));

    // Teardown order is load-bearing. OrderServer is the SOLE producer of
    // client_requests, and MatchingEngine::stop() (invoked by ~MatchingEngine)
    // writes the SHUTDOWN pill into that same SPSC queue from THIS thread.
    // The OrderServer thread must therefore be quiesced first, or the queue
    // briefly has two concurrent producers. After the pill: the matching
    // engine thread exits, then the publisher (sole consumer of
    // market_updates) is free to go, then the logger.
    delete order_server;          order_server          = nullptr;
    delete matching_engine;       matching_engine       = nullptr;
    delete market_data_publisher; market_data_publisher = nullptr;
    delete logger;                logger                = nullptr;

    return EXIT_SUCCESS;
}
