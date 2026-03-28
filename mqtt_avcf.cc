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

    /* -- call_end --------------------------------------------------------- */

    int call_end(Call_Data_t call_info) override {
        if (!write_enabled_ && !mqtt_enabled_) return 0;
        if (call_info.filename.empty()) return 0;

        // Filter: skip digital calls if analog_only is set
        if (analog_only_ && !call_info.audio_type.empty() && call_info.audio_type != "analog")
            return 0;

        // Read the audio file
        std::ifstream audio_file(call_info.filename, std::ios::binary | std::ios::ate);
        if (!audio_file.is_open()) {
            BOOST_LOG_TRIVIAL(error) << TAG << "Cannot open " << call_info.filename;
            return 0;
        }
        size_t audio_size = audio_file.tellg();
        audio_file.seekg(0);
        std::vector<uint8_t> audio_bytes(audio_size);
        audio_file.read(reinterpret_cast<char *>(audio_bytes.data()), audio_size);
        audio_file.close();

        std::string content_type = detect_content_type(call_info.filename);
        uint32_t call_id = next_id_++;

        // Build .avcf in memory
        std::vector<uint8_t> avcf;
        avcf.reserve(audio_size + 1024); // audio + overhead for headers/metadata

        // 1. CALL_START
        {
            std::string sys = call_info.short_name;
            uint8_t nlen = static_cast<uint8_t>(std::min(sys.size(), (size_t)255));
            sssp_header_t hdr; fill_header(hdr, SSSP_MSG_CALL_START, sizeof(sssp_call_start_t) + nlen);
            append(avcf, &hdr, sizeof(hdr));
            sssp_call_start_t cs{};
            cs.talkgroup = (uint32_t)call_info.talkgroup;
            cs.frequency_hz = (uint64_t)call_info.freq;
            cs.timestamp_us = (uint64_t)call_info.start_time * 1000000ULL;
            cs.call_id = call_id;
            cs.system_name_len = nlen;
            append(avcf, &cs, sizeof(cs));
            if (nlen) append(avcf, sys.data(), nlen);
        }

        // 2. AUDIO_DATA
        {
            uint8_t ct_len = static_cast<uint8_t>(std::min(content_type.size(), (size_t)255));
            uint32_t payload_len = 1 + ct_len + static_cast<uint32_t>(audio_size);
            sssp_header_t hdr; fill_header(hdr, SSSP_MSG_AUDIO_DATA, payload_len);
            append(avcf, &hdr, sizeof(hdr));
            append(avcf, &ct_len, 1);
            append(avcf, content_type.data(), ct_len);
            append(avcf, audio_bytes.data(), audio_size);
        }

        // 3. CALL_METADATA (JSON)
        {
            nlohmann::ordered_json meta;
            if (!call_info.talkgroup_tag.empty())        meta["tg_tag"] = call_info.talkgroup_tag;
            if (!call_info.talkgroup_alpha_tag.empty())   meta["tg_alpha_tag"] = call_info.talkgroup_alpha_tag;
            if (!call_info.talkgroup_group.empty())       meta["tg_group"] = call_info.talkgroup_group;
            if (!call_info.talkgroup_description.empty()) meta["tg_description"] = call_info.talkgroup_description;
            meta["signal"] = call_info.signal;
            meta["noise"] = call_info.noise;
            meta["freq_error"] = call_info.freq_error;
            meta["spike_count"] = call_info.spike_count;
            meta["emergency"] = call_info.emergency;
            meta["priority"] = call_info.priority;
            meta["phase2_tdma"] = call_info.phase2_tdma;
            meta["tdma_slot"] = call_info.tdma_slot;
            if (!call_info.patched_talkgroups.empty()) {
                nlohmann::ordered_json ptgs = nlohmann::ordered_json::array();
                for (const auto tg : call_info.patched_talkgroups) ptgs.push_back(tg);
                meta["patched_tgs"] = ptgs;
            }
            if (!call_info.transmission_source_list.empty()) {
                nlohmann::ordered_json src_list = nlohmann::ordered_json::array();
                for (const auto &src : call_info.transmission_source_list) {
                    src_list.push_back({
                        {"src", src.source}, {"time", src.time}, {"pos", src.position},
                        {"emergency", src.emergency}, {"signal_system", src.signal_system},
                        {"tag", src.tag}
                    });
                }
                meta["src_list"] = src_list;
            }
            std::string json_str = meta.dump();
            uint32_t json_len = static_cast<uint32_t>(json_str.size());
            sssp_header_t hdr; fill_header(hdr, SSSP_MSG_CALL_METADATA, json_len);
            append(avcf, &hdr, sizeof(hdr));
            append(avcf, json_str.data(), json_len);
        }

        // 4. CALL_END
        {
            std::string sys = call_info.short_name;
            uint8_t nlen = static_cast<uint8_t>(std::min(sys.size(), (size_t)255));
            sssp_header_t hdr; fill_header(hdr, SSSP_MSG_CALL_END, sizeof(sssp_call_end_t) + nlen);
            append(avcf, &hdr, sizeof(hdr));
            sssp_call_end_t ce{};
            ce.talkgroup = (uint32_t)call_info.talkgroup;
            ce.call_id = call_id;
            ce.src_id = (uint32_t)call_info.source_num;
            ce.frequency_hz = (uint64_t)call_info.freq;
            ce.duration_ms = (uint32_t)(call_info.length * 1000.0);
            ce.error_count = (uint32_t)call_info.error_count;
            ce.encrypted = call_info.encrypted ? 1 : 0;
            ce.system_name_len = nlen;
            append(avcf, &ce, sizeof(ce));
            if (nlen) append(avcf, sys.data(), nlen);
        }

        // Write .avcf sidecar file
        if (write_enabled_) {
            std::string avcf_path = wav_to_avcf(call_info.filename);
            std::ofstream out(avcf_path, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char *>(avcf.data()), avcf.size());
                if (out.good()) {
                    BOOST_LOG_TRIVIAL(info) << TAG << "Wrote " << avcf_path
                        << " (" << avcf.size() << " bytes)";
                } else {
                    BOOST_LOG_TRIVIAL(error) << TAG << "Write failed: " << avcf_path;
                }
                out.close();
            } else {
                BOOST_LOG_TRIVIAL(error) << TAG << "Cannot create " << avcf_path;
            }
        }

        // Publish over MQTT
        if (mqtt_enabled_ && mqtt_connected_) {
            std::string b64 = bytes_to_base64(avcf);

            nlohmann::ordered_json src_list = nlohmann::ordered_json::array();
            for (const auto &src : call_info.transmission_source_list) {
                src_list.push_back({
                    {"src", src.source}, {"time", src.time}, {"pos", src.position},
                    {"emergency", src.emergency}, {"signal_system", src.signal_system},
                    {"tag", src.tag}
                });
            }

            nlohmann::ordered_json payload = {
                {"audio_avcf_base64", b64},
                {"metadata", {
                    {"talkgroup", call_info.talkgroup}, {"talkgroup_tag", call_info.talkgroup_tag},
                    {"talkgroup_alpha_tag", call_info.talkgroup_alpha_tag},
                    {"talkgroup_group", call_info.talkgroup_group},
                    {"freq", call_info.freq}, {"start_time", call_info.start_time},
                    {"stop_time", call_info.stop_time},
                    {"call_length", call_info.stop_time - call_info.start_time},
                    {"signal", call_info.signal}, {"noise", call_info.noise},
                    {"freq_error", call_info.freq_error}, {"spike_count", call_info.spike_count},
                    {"emergency", call_info.emergency}, {"priority", call_info.priority},
                    {"phase2_tdma", call_info.phase2_tdma}, {"tdma_slot", call_info.tdma_slot},
                    {"analog", true}, {"audio_type", call_info.audio_type},
                    {"short_name", call_info.short_name},
                    {"filename", basename_of(call_info.filename)},
                    {"srcList", src_list}
                }}
            };

            if (!call_info.patched_talkgroups.empty()) {
                nlohmann::ordered_json ptgs = nlohmann::ordered_json::array();
                for (const auto tg : call_info.patched_talkgroups) ptgs.push_back(tg);
                payload["metadata"]["patched_talkgroups"] = ptgs;
            }

            std::string pub_topic = topic_ + "/avcf";
            try {
                mqtt_client_->publish(mqtt::message_ptr_builder()
                    .topic(pub_topic).payload(payload.dump())
                    .qos(qos_).retained(false).finalize());
                BOOST_LOG_TRIVIAL(info) << TAG << "Published TG " << call_info.talkgroup
                    << " (" << b64.size() << " b64 bytes) -> " << pub_topic;
            } catch (const mqtt::exception &e) {
                BOOST_LOG_TRIVIAL(error) << TAG << "Publish failed: " << e.what();
            }
        }

        return 0;
    }

    static boost::shared_ptr<Avcf_Handler> create() {
        return boost::shared_ptr<Avcf_Handler>(new Avcf_Handler());
    }
};

BOOST_DLL_ALIAS(Avcf_Handler::create, create_plugin)
