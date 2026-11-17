#pragma once
#include "annexb.hpp"
#include "settings.hpp"
#include "video.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
namespace httplib::ws {
class WebSocket;
}
namespace stream {
class Capture;
class Input;
struct Session;
class Server {
    std::unique_ptr<stream::Capture> capture_;
    std::shared_ptr<stream::Input> input_;
    const unsigned bitrate_;
    const std::string config_path_;
    stream::SettingsStore settings_;
    stream::Resolution resolution_, active_resolution_;
    // Guards peer_, frames_, resolution fields and error_.
    std::mutex mutex_;
    std::condition_variable ready_;
    std::shared_ptr<Session> peer_;
    struct Frame {
        stream::Bytes bytes;
        uint32_t timestamp = 0;
    };
    std::deque<Frame> frames_;
    std::atomic<bool> quit_{false}, streaming_{false}, resync_{false};
    std::atomic<uint64_t> captured_{0}, sent_{0}, dropped_{0}, bytes_{0};
    std::atomic<int64_t> last_frame_{0};
    std::string error_;
    std::jthread worker_;

    void run();
    std::shared_ptr<Session> accept_offer(const std::string& encoded);
    static void add_candidate(const std::shared_ptr<Session>& peer, const nlohmann::json& value);

  public:
    Server(unsigned bitrate, const std::string& config_path);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    [[nodiscard]] nlohmann::json status();
    [[nodiscard]] nlohmann::json video_state();
    void signaling(httplib::ws::WebSocket& socket);
    void stop() noexcept;
};
} // namespace stream
