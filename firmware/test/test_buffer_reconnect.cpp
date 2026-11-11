// ============================================================================
// Offline Buffer / MQTT Reconnect Tests (Step 4 — Halim Master Audit, §12)
//
// Required behavior:
//   connected    -> publish
//   disconnected -> buffer locally
//   reconnected  -> publish backlog in order
//
// Cases: bounded storage, ordering, malformed record, storage full,
//        reconnect, duplicate prevention, restart recovery.
//
// Build:
//   g++ -std=c++17 -I firmware/include firmware/test/test_buffer_reconnect.cpp -o buffer_reconnect.exe
// ============================================================================
#include <iostream>
#include <cassert>
#include <string>
#include <vector>

#include "../src/storage/mock_spiffs_buffer.hpp"
#include "../src/storage/telemetry_dispatcher.hpp"

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    if (ok) {
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        g_failures++;
    }
}

class FakeComms : public ICommsDriver {
public:
    bool connected{true};
    int fail_at_publish_call{-1}; // -1 = never fail; N = fail on the Nth publish attempt
    int publish_calls{0};
    std::vector<std::string> published;

    bool isConnected() const override { return connected; }

    bool publishTelemetry(const std::string& payload) override {
        if (!connected) return false;
        publish_calls++;
        if (fail_at_publish_call > 0 && publish_calls == fail_at_publish_call) {
            return false;
        }
        published.push_back(payload);
        return true;
    }
};

static void test_publish_when_connected() {
    FakeComms comms;
    MockSPIFFSStorageBuffer buffer(10);
    TelemetryDispatcher dispatcher(comms, buffer);

    bool published = dispatcher.handleRecord("rec_1");
    check("connected: record published immediately, buffer stays empty",
          published && comms.published.size() == 1 && !buffer.hasBacklog());
}

static void test_buffer_when_disconnected_preserves_order() {
    FakeComms comms;
    comms.connected = false;
    MockSPIFFSStorageBuffer buffer(10);
    TelemetryDispatcher dispatcher(comms, buffer);

    dispatcher.handleRecord("rec_1");
    dispatcher.handleRecord("rec_2");
    dispatcher.handleRecord("rec_3");

    std::string first, second, third;
    bool order_ok = buffer.peekOldest(first) && first == "rec_1";
    buffer.removeOldest();
    order_ok = order_ok && buffer.peekOldest(second) && second == "rec_2";
    buffer.removeOldest();
    order_ok = order_ok && buffer.peekOldest(third) && third == "rec_3";

    check("disconnected: records buffered in FIFO order",
          order_ok && comms.published.empty() && buffer.getRecordCount() == 1);
}

static void test_reconnect_drains_backlog_in_order() {
    FakeComms comms;
    comms.connected = false;
    MockSPIFFSStorageBuffer buffer(10);
    TelemetryDispatcher dispatcher(comms, buffer);

    dispatcher.handleRecord("rec_1");
    dispatcher.handleRecord("rec_2");
    dispatcher.handleRecord("rec_3");

    comms.connected = true;
    size_t drained = dispatcher.drainBacklog(10);

    bool ok = drained == 3 &&
              comms.published.size() == 3 &&
              comms.published[0] == "rec_1" &&
              comms.published[1] == "rec_2" &&
              comms.published[2] == "rec_3" &&
              !buffer.hasBacklog();
    check("reconnect: backlog drained oldest-first, buffer emptied", ok);
}

static void test_malformed_record_rejected() {
    FakeComms comms;
    MockSPIFFSStorageBuffer buffer(10);
    TelemetryDispatcher dispatcher(comms, buffer);

    bool result = dispatcher.handleRecord("");
    check("malformed (empty) record rejected: not published, not buffered",
          !result && comms.published.empty() && !buffer.hasBacklog());
}

static void test_bounded_storage_drop_oldest() {
    FakeComms comms;
    comms.connected = false;
    MockSPIFFSStorageBuffer buffer(3); // bounded capacity
    TelemetryDispatcher dispatcher(comms, buffer);

    for (int i = 1; i <= 5; ++i) {
        dispatcher.handleRecord("rec_" + std::to_string(i));
    }

    // Drop-oldest policy keeps the newest 3 records
    comms.connected = true;
    dispatcher.drainBacklog(10);
    bool ok = comms.published.size() == 3 &&
              comms.published[0] == "rec_3" &&
              comms.published[1] == "rec_4" &&
              comms.published[2] == "rec_5";
    check("storage full: bounded drop-oldest policy keeps newest records", ok);
}

static void test_duplicate_prevention_and_mid_drain_failure() {
    FakeComms comms;
    comms.connected = false;
    MockSPIFFSStorageBuffer buffer(10);
    TelemetryDispatcher dispatcher(comms, buffer);

    dispatcher.handleRecord("rec_1");
    dispatcher.handleRecord("rec_2");
    dispatcher.handleRecord("rec_3");

    // Reconnect; the 2nd publish attempt fails mid-drain
    comms.connected = true;
    comms.fail_at_publish_call = 2;
    size_t first_drain = dispatcher.drainBacklog(10);

    bool stopped_correctly = first_drain == 1 &&
                             comms.published.size() == 1 &&
                             comms.published[0] == "rec_1" &&
                             buffer.getRecordCount() == 2;

    // Recover: next drain publishes rec_2 and rec_3 exactly once each
    comms.fail_at_publish_call = -1;
    size_t final_drain = dispatcher.drainBacklog(10);

    bool ok = stopped_correctly &&
              final_drain == 2 &&
              comms.published.size() == 3 &&
              comms.published[0] == "rec_1" &&
              comms.published[1] == "rec_2" &&
              comms.published[2] == "rec_3" &&
              !buffer.hasBacklog();
    check("mid-drain failure: no loss, no duplicate, order preserved", ok);
}

static void test_no_republish_after_success() {
    FakeComms comms;
    MockSPIFFSStorageBuffer buffer(10);
    TelemetryDispatcher dispatcher(comms, buffer);

    dispatcher.handleRecord("rec_1"); // published live
    size_t extra = dispatcher.drainBacklog(10);
    check("successful publish never re-enters the buffer (no duplicate)",
          extra == 0 && comms.published.size() == 1);
}

static void test_restart_recovery() {
    FakeComms comms;
    comms.connected = false;
    MockSPIFFSStorageBuffer buffer(10); // persists across "restart"

    {
        TelemetryDispatcher dispatcher_before_restart(comms, buffer);
        dispatcher_before_restart.handleRecord("rec_1");
        dispatcher_before_restart.handleRecord("rec_2");
    } // dispatcher destroyed; buffer survives (simulated reboot)

    comms.connected = true;
    TelemetryDispatcher dispatcher_after_restart(comms, buffer);
    size_t drained = dispatcher_after_restart.drainBacklog(10);

    bool ok = drained == 2 &&
              comms.published[0] == "rec_1" &&
              comms.published[1] == "rec_2";
    check("restart recovery: new dispatcher drains surviving backlog in order", ok);
}

static void test_drain_stops_when_disconnected() {
    FakeComms comms;
    comms.connected = false;
    MockSPIFFSStorageBuffer buffer(10);
    TelemetryDispatcher dispatcher(comms, buffer);
    dispatcher.handleRecord("rec_1");

    size_t drained = dispatcher.drainBacklog(10);
    check("drain is a no-op while disconnected (record retained)",
          drained == 0 && buffer.getRecordCount() == 1);
}

int main() {
    std::cout << "\n================================================\n";
    std::cout << " OFFLINE BUFFER / MQTT RECONNECT TESTS (STEP 4)\n";
    std::cout << "================================================\n";

    test_publish_when_connected();
    test_buffer_when_disconnected_preserves_order();
    test_reconnect_drains_backlog_in_order();
    test_malformed_record_rejected();
    test_bounded_storage_drop_oldest();
    test_duplicate_prevention_and_mid_drain_failure();
    test_no_republish_after_success();
    test_restart_recovery();
    test_drain_stops_when_disconnected();

    if (g_failures == 0) {
        std::cout << "\n[BUFFER/RECONNECT: ALL CHECKS PASSED]\n\n";
        return 0;
    }
    std::cout << "\n[BUFFER/RECONNECT: " << g_failures << " FAILURE(S)]\n\n";
    return 1;
}
