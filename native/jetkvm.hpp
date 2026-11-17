#pragma once
#include "input.hpp"
#include "settings.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace stream {
// JetKVM local JSON-RPC 2.0 and HID RPC v1, independent of its device firmware.
class JetKvmProtocol {
  public:
    using Json = nlohmann::json;
    using Binary = std::vector<uint8_t>;
    using SendJson = std::function<void(const Json&)>;
    using SendBinary = std::function<void(const Binary&)>;

  private:
    std::shared_ptr<Input> input_;
    SettingsStore settings_;
    std::function<Json()> video_;
    SendJson rpc_;
    SendBinary hid_;
    // Guards input state, coordinates, key watchdog and outgoing RPC/HID callbacks.
    std::mutex state_mutex_;
    std::set<unsigned> keys_;
    unsigned buttons_ = 0;
    double x_ = .5, y_ = .5;
    std::chrono::steady_clock::time_point last_key_{};
    struct Step {
        unsigned modifier = 0;
        Binary keys;
        unsigned delay = 0;
    };
    struct Macro {
        std::vector<Step> steps;
        bool paste = false;
        uint64_t generation = 0;
    };
    // Guards the pending macro queue; state_mutex_ is never acquired while held.
    std::mutex macro_mutex_;
    std::condition_variable macro_ready_;
    std::deque<Macro> macros_;
    std::atomic<bool> quit_{false}, macro_running_{false};
    std::atomic<uint64_t> generation_{0};
    // Serializes stop and guards stopped_.
    std::mutex stop_mutex_;
    bool stopped_ = false;
    std::jthread macro_worker_;
    void key_locked(unsigned usage, bool down);
    void keyboard_locked(unsigned modifier, const Binary& keys);
    void pointer_locked(double x, double y, unsigned buttons);
    void buttons_locked(unsigned buttons);
    void key_state_locked();
    Json down_state_locked() const;
    Json call(const std::string& method, const Json& params);
    void run_macros();
    void cancel_macro();

  public:
    JetKvmProtocol(std::shared_ptr<Input> input, std::string config, std::function<Json()> video);
    ~JetKvmProtocol();
    JetKvmProtocol(const JetKvmProtocol&) = delete;
    JetKvmProtocol& operator=(const JetKvmProtocol&) = delete;
    JetKvmProtocol(JetKvmProtocol&&) = delete;
    JetKvmProtocol& operator=(JetKvmProtocol&&) = delete;
    void bind_rpc(SendJson send);
    void bind_hid(SendBinary send);
    void rpc(const std::string& message);
    void hid(const Binary& message, bool reliable = true);
    void tick();
    void release();
    void stop();
};
} // namespace stream
