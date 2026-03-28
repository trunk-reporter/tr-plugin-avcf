/*
 * mqtt_avcf.cc — Analog Voice Capture Format (.avcf) writer + MQTT publisher.
 *
 * A trunk-recorder plugin that captures analog (conventional FM) calls as
 * self-contained .avcf files with embedded audio + metadata, and optionally
 * publishes them to an MQTT broker.
 *
 * Unlike mqtt_dvcf (incremental streaming), this plugin does all work at
 * call_end — reads the completed audio file, wraps it in AVCF framing,
 * writes the sidecar, and publishes.
 *
 * Config:
 *   {
 *     "name": "mqtt_avcf",
 *     "library": "libmqtt_avcf",
 *     "write_enabled": true,
 *     "mqtt_enabled": false,
 *     "analog_only": true,
 *     "broker": "tcp://localhost:1883",
 *     "topic": "trunk-recorder",
 *     "clientid": "avcf-handler",
 *     "username": "",
 *     "password": "",
 *     "qos": 0
 *   }
 */

#include "../../trunk-recorder/plugin_manager/plugin_api.h"

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/dll/alias.hpp>
#include <boost/log/trivial.hpp>

#include <mqtt/async_client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static const std::string TAG = "[mqtt_avcf] ";

/* -- SymbolStream Protocol v2 constants ---------------------------------- */

static constexpr uint8_t  SSSP_MAGIC_0          = 0x53; // 'S'
static constexpr uint8_t  SSSP_MAGIC_1          = 0x59; // 'Y'
static constexpr uint8_t  SSSP_VERSION           = 0x02;
static constexpr uint8_t  SSSP_MSG_CALL_START    = 0x02;
static constexpr uint8_t  SSSP_MSG_CALL_END      = 0x03;
static constexpr uint8_t  SSSP_MSG_CALL_METADATA = 0x05;
static constexpr uint8_t  SSSP_MSG_AUDIO_DATA    = 0x06;

/* -- Packed binary structures -------------------------------------------- */

#pragma pack(push, 1)

struct sssp_header_t {
    uint8_t  magic[2];
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t payload_len;
};

struct sssp_call_start_t {
    uint32_t talkgroup;
    uint64_t frequency_hz;
    uint64_t timestamp_us;
    uint32_t call_id;
    uint8_t  system_name_len;
};

struct sssp_call_end_t {
    uint32_t talkgroup;
    uint32_t call_id;
    uint32_t src_id;
    uint64_t frequency_hz;
    uint32_t duration_ms;
    uint32_t error_count;
    uint8_t  encrypted;
    uint8_t  system_name_len;
};

#pragma pack(pop)

static_assert(sizeof(sssp_header_t) == 8, "header must be 8 bytes");

/* -- Helpers -------------------------------------------------------------- */

static void fill_header(sssp_header_t &h, uint8_t msg_type, uint32_t payload_len) {
    h.magic[0] = SSSP_MAGIC_0; h.magic[1] = SSSP_MAGIC_1;
    h.version = SSSP_VERSION; h.msg_type = msg_type; h.payload_len = payload_len;
}

static void append(std::vector<uint8_t> &buf, const void *data, size_t len) {
    auto p = static_cast<const uint8_t *>(data);
    buf.insert(buf.end(), p, p + len);
}

static std::string wav_to_avcf(const std::string &p) {
    auto dot = p.rfind('.');
    return (dot != std::string::npos ? p.substr(0, dot) : p) + ".avcf";
}

static std::string basename_of(const std::string &p) {
    auto pos = p.rfind('/');
    if (pos == std::string::npos) pos = p.rfind('\\');
    return (pos != std::string::npos) ? p.substr(pos + 1) : p;
}

static std::string detect_content_type(const std::string &filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = filename.substr(dot);
    if (ext == ".wav")  return "audio/wav";
    if (ext == ".m4a")  return "audio/mp4";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".ogg")  return "audio/ogg";
    return "application/octet-stream";
}

static std::string bytes_to_base64(const std::vector<uint8_t> &buf) {
    using namespace boost::archive::iterators;
    using It = base64_from_binary<transform_width<std::vector<uint8_t>::const_iterator, 6, 8>>;
    std::string b64(It(buf.begin()), It(buf.end()));
    b64.append((3 - buf.size() % 3) % 3, '=');
    return b64;
}

/* -- Plugin class --------------------------------------------------------- */

class Avcf_Handler : public Plugin_Api, public virtual mqtt::callback {

    // Config -- features
    bool write_enabled_ = true;
    bool mqtt_enabled_  = false;
    bool analog_only_   = true;

    // Config -- MQTT
    std::string broker_, topic_, client_id_, username_, password_;
    int qos_ = 0;

    // MQTT runtime
    std::unique_ptr<mqtt::async_client> mqtt_client_;
    std::atomic<bool> mqtt_connected_{false};

    // Call ID counter
    uint32_t next_id_ = 1;

    /* -- MQTT helpers ---------------------------------------------------- */

    void mqtt_connect() {
        auto ssl = mqtt::ssl_options_builder().verify(false).enable_server_cert_auth(false).finalize();
        auto opts = mqtt::connect_options_builder().clean_session()
            .ssl(ssl).automatic_reconnect(std::chrono::seconds(10), std::chrono::seconds(40)).finalize();
        if (!username_.empty() && !password_.empty()) {
            opts.set_user_name(username_); opts.set_password(password_);
        }
        mqtt_client_ = std::make_unique<mqtt::async_client>(broker_, client_id_);
        mqtt_client_->set_callback(*this);
        try {
            BOOST_LOG_TRIVIAL(info) << TAG << "Connecting to " << broker_;
            mqtt_client_->connect(opts)->wait();
        } catch (const mqtt::exception &e) {
            BOOST_LOG_TRIVIAL(error) << TAG << "MQTT connect failed: " << e.what();
        }
    }

    /* -- mqtt::callback -------------------------------------------------- */

    void connected(const std::string &) override {
        BOOST_LOG_TRIVIAL(info) << TAG << "MQTT connected to " << broker_;
        mqtt_connected_ = true;
    }
    void connection_lost(const std::string &cause) override {
        BOOST_LOG_TRIVIAL(error) << TAG << "MQTT connection lost: " << cause;
        mqtt_connected_ = false;
    }

public:
    Avcf_Handler() {}
    ~Avcf_Handler() override = default;

    /* -- Plugin_Api ------------------------------------------------------ */

    int parse_config(json config_data) override {
        write_enabled_ = config_data.value("write_enabled", true);
        mqtt_enabled_  = config_data.value("mqtt_enabled", false);
        analog_only_   = config_data.value("analog_only", true);
        broker_    = config_data.value("broker", "tcp://localhost:1883");
        topic_     = config_data.value("topic", "trunk-recorder");
        client_id_ = config_data.value("clientid", "avcf-handler");
        username_  = config_data.value("username", "");
        password_  = config_data.value("password", "");
        qos_       = config_data.value("qos", 0);
        if (!topic_.empty() && topic_.back() == '/') topic_.pop_back();

        BOOST_LOG_TRIVIAL(info) << TAG << "write_enabled=" << write_enabled_
            << " mqtt_enabled=" << mqtt_enabled_ << " analog_only=" << analog_only_;
        if (mqtt_enabled_)
            BOOST_LOG_TRIVIAL(info) << TAG << "broker=" << broker_
                << " topic=" << topic_ << "/avcf qos=" << qos_;
        return 0;
    }

    int start() override {
        if (mqtt_enabled_) mqtt_connect();
        BOOST_LOG_TRIVIAL(info) << TAG << "Started";
        return 0;
    }

    int stop() override {
        if (mqtt_client_ && mqtt_connected_) {
            try { mqtt_client_->disconnect()->wait_for(std::chrono::seconds(5)); }
            catch (...) {}
            mqtt_connected_ = false;
        }
        mqtt_client_.reset();
        return 0;
    }

    // Task 4 will add call_end() here and close the class.
