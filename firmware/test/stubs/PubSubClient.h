#pragma once
// PubSubClient-compatible fake for native testing of the MQTT comms driver.
// Mirrors the subset of the real PubSubClient API used by Esp32MqttCommsT.
#include <cstdint>
#include <string>
#include <vector>

struct PublishedMessage {
    std::string topic;
    std::string payload;
    bool retained;
};

struct ConnectCall {
    std::string client_id;
    std::string username;
    std::string password;
    std::string will_topic;
    uint8_t will_qos;
    bool will_retain;
    std::string will_message;
    bool anonymous;
};

class FakeMqttClient {
public:
    using Callback = void (*)(char*, uint8_t*, unsigned int);

    bool connected_flag{false};
    bool connect_result{true};
    bool publish_result{true};
    int loop_calls{0};

    std::string server_host;
    uint16_t server_port{0};
    Callback callback{nullptr};
    std::vector<ConnectCall> connect_calls;
    std::vector<PublishedMessage> published;
    std::vector<std::string> subscriptions;

    void setServer(const char* host, uint16_t port) {
        server_host = host ? host : "";
        server_port = port;
    }

    void setCallback(Callback cb) { callback = cb; }

    bool subscribe(const char* topic, uint8_t qos = 0) {
        (void)qos;
        if (!connected_flag) {
            return false;
        }
        subscriptions.push_back(topic ? topic : "");
        return true;
    }

    // Test-only helper: deliver an inbound message like PubSubClient::loop() would.
    void deliver(const char* topic, const std::string& payload) {
        if (!callback) {
            return;
        }
        std::vector<uint8_t> buffer(payload.begin(), payload.end());
        callback(const_cast<char*>(topic), buffer.data(),
                 static_cast<unsigned int>(buffer.size()));
    }

    bool connected() { return connected_flag; }

    bool connect(const char* client_id) {
        connect_calls.push_back({client_id ? client_id : "", "", "", "", 0, false, "", true});
        if (connect_result) {
            connected_flag = true;
        }
        return connect_result;
    }

    bool connect(const char* client_id, const char* willTopic, uint8_t willQos,
                 bool willRetain, const char* willMessage) {
        connect_calls.push_back({client_id ? client_id : "",
                                 "", "",
                                 willTopic ? willTopic : "",
                                 willQos,
                                 willRetain,
                                 willMessage ? willMessage : "",
                                 true});
        if (connect_result) {
            connected_flag = true;
        }
        return connect_result;
    }

    bool connect(const char* client_id, const char* username, const char* password,
                 const char* willTopic, uint8_t willQos, bool willRetain, const char* willMessage) {
        connect_calls.push_back({client_id ? client_id : "",
                                 username ? username : "",
                                 password ? password : "",
                                 willTopic ? willTopic : "",
                                 willQos,
                                 willRetain,
                                 willMessage ? willMessage : "",
                                 false});
        if (connect_result) {
            connected_flag = true;
        }
        return connect_result;
    }

    bool publish(const char* topic, const char* payload, bool retained) {
        if (!connected_flag || !publish_result) {
            return false;
        }
        published.push_back({topic ? topic : "", payload ? payload : "", retained});
        return true;
    }

    bool loop() {
        loop_calls++;
        return connected_flag;
    }

    int state() { return connected_flag ? 0 : -1; }
};
