#include "server.hpp"

#include "capture.hpp"
#include "input.hpp"
#include "jetkvm.hpp"

#include <algorithm>
#include <httplib.h>
#include <iostream>
#include <openssl/evp.h>
#include <rtc/rtc.hpp>
#include <sys/resource.h>

using json = nlohmann::json;
using namespace std::chrono_literals;
namespace stream {
static int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static std::string base64(const std::string& input) {
    std::string out(4 * ((input.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
        reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
    return out;
}
static std::string unbase64(const std::string& input) {
    if (input.empty() || input.size() > 128UL * 1024UL || input.size() % 4 ||
        input.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
            std::string::npos) {
        throw std::invalid_argument("Invalid session description encoding");
    }
    std::string out(input.size() / 4 * 3, '\0');
    const auto size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
        reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
    if (size < 0) {
        throw std::invalid_argument("Invalid session description encoding");
    }
    out.resize(size - (input.back() == '=') - (input.size() > 1 && input[input.size() - 2] == '='));
    return out;
}
struct Session {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<stream::JetKvmProtocol> protocol;
    std::vector<std::shared_ptr<rtc::DataChannel>> channels;
    std::atomic<bool> connected{false}, ended{false}, idr{true};
    // Guards signaling queues, description ordering and the channel collection.
    std::mutex mutex;
    std::deque<json> signals, candidates;
    bool description_queued = false;
};
static void bind_channel(
    const std::shared_ptr<Session>& p, const std::shared_ptr<rtc::DataChannel>& channel) {
    if (p->ended) {
        return;
    }
    const auto label = channel->label();
    if (label != "rpc" && label != "hidrpc" && label != "hidrpc-unreliable-ordered" &&
        label != "hidrpc-unreliable-nonordered") {
        channel->close();
        return;
    }
    {
        const std::scoped_lock lock(p->mutex);
        p->channels.push_back(channel);
    }
    const auto protocol = std::weak_ptr<stream::JetKvmProtocol>(p->protocol);
    const auto data = std::weak_ptr<rtc::DataChannel>(channel);
    const bool rpc = label == "rpc";
    const bool reliable_hid = label == "hidrpc";
    channel->onOpen([protocol, data, rpc, reliable_hid] {
        auto handler = protocol.lock();
        if (!handler) {
            return;
        }
        if (rpc) {
            handler->bind_rpc([data](const json& value) {
                if (auto ch = data.lock(); ch && ch->isOpen()) {
                    try {
                        ch->send(value.dump());
                    } catch (const std::exception& error) {
                        std::cerr << "Data channel send: " << error.what() << '\n';
                    }
                }
            });
        }
        if (reliable_hid) {
            handler->bind_hid([data](const stream::JetKvmProtocol::Binary& value) {
                if (auto ch = data.lock(); ch && ch->isOpen()) {
                    try {
                        const auto bytes = std::as_bytes(std::span(value));
                        ch->send(bytes.data(), bytes.size());
                    } catch (const std::exception& error) {
                        std::cerr << "Data channel send: " << error.what() << '\n';
                    }
                }
            });
        }
    });
    if (label == "rpc" || label == "hidrpc") {
        channel->onClosed([protocol] {
            if (auto handler = protocol.lock()) {
                handler->release();
            }
        });
    }
    channel->onMessage([protocol, rpc, reliable_hid](rtc::message_variant message) {
        auto handler = protocol.lock();
        if (!handler) {
            return;
        }
        try {
            if (rpc && std::holds_alternative<std::string>(message)) {
                const auto& text = std::get<std::string>(message);
                if (text.size() <= 128UL * 1024UL) {
                    handler->rpc(text);
                }
            } else if (!rpc && std::holds_alternative<rtc::binary>(message)) {
                const auto& bytes = std::get<rtc::binary>(message);
                stream::JetKvmProtocol::Binary input(bytes.size());
                std::ranges::transform(bytes, input.begin(),
                    [](std::byte value) { return std::to_integer<uint8_t>(value); });
                handler->hid(input, reliable_hid);
            }
        } catch (const std::exception& e) {
            std::cerr << "JetKVM input: " << e.what() << '\n';
        }
    });
}

static void configure_video(
    const std::shared_ptr<Session>& peer, rtc::Description& offer, unsigned bitrate) {
    const auto weak = std::weak_ptr<Session>(peer);
    for (int i = 0; i < offer.mediaCount(); ++i) {
        auto entry = offer.media(i);
        if (!std::holds_alternative<rtc::Description::Media*>(entry)) {
            continue;
        }
        auto media = *std::get<rtc::Description::Media*>(entry);
        if (media.type() != "video" || peer->track) {
            media.setDirection(rtc::Description::Direction::Inactive);
            (void)peer->pc->addTrack(media);
            continue;
        }
        int payload = -1;
        for (auto pt : media.payloadTypes()) {
            const auto codec = media.rtpMap(pt);
            if (codec && codec->format == "H264") {
                bool packetized = false;
                bool baseline = false;
                for (const auto& fmtp : codec->fmtps) {
                    packetized |= fmtp.find("packetization-mode=1") != std::string::npos;
                    baseline |= fmtp.find("profile-level-id=42") != std::string::npos;
                }
                if (packetized && (payload < 0 || baseline)) {
                    payload = pt;
                    if (baseline) {
                        break;
                    }
                }
            }
        }
        if (payload < 0) {
            throw std::invalid_argument("Browser did not offer H.264 packetization mode 1");
        }
        for (auto pt : media.payloadTypes()) {
            if (pt != payload) {
                media.removeRtpMap(pt);
            }
        }
        media.setDirection(rtc::Description::Direction::SendOnly);
        media.addSSRC(42, "webos", "webos", "screen");
        media.setBitrate(static_cast<int>(bitrate));
        peer->track = peer->pc->addTrack(media);
        auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(
            42, "webos", static_cast<uint8_t>(payload), 90000);
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtp, 1150);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak] {
            if (auto p = weak.lock()) {
                p->idr = true;
            }
        }));
        peer->track->setMediaHandler(packetizer);
    }
    if (!peer->track) {
        throw std::invalid_argument("Offer has no video track");
    }
}

static void configure_signaling(const std::shared_ptr<Session>& peer) {
    const auto weak = std::weak_ptr<Session>(peer);
    peer->pc->onStateChange([weak](rtc::PeerConnection::State state) {
        if (auto p = weak.lock()) {
            p->connected = state == rtc::PeerConnection::State::Connected;
            if (state == rtc::PeerConnection::State::Closed ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Disconnected) {
                p->ended = true;
            }
        }
    });
    peer->pc->onLocalDescription([weak](const rtc::Description& description) {
        if (auto p = weak.lock()) {
            const std::scoped_lock lock(p->mutex);
            p->signals.push_back({{"type", "answer"},
                {"data",
                    base64(json{{"type", "answer"}, {"sdp", std::string(description)}}.dump())}});
            p->description_queued = true;
            for (auto& candidate : p->candidates) {
                p->signals.push_back(std::move(candidate));
            }
            p->candidates.clear();
        }
    });
    peer->pc->onLocalCandidate([weak](const rtc::Candidate& candidate) {
        if (auto p = weak.lock()) {
            json message = {{"type", "new-ice-candidate"},
                {"data", {{"candidate", std::string(candidate)}, {"sdpMid", candidate.mid()}}}};
            const std::scoped_lock lock(p->mutex);
            auto& queue = p->description_queued ? p->signals : p->candidates;
            if (queue.size() < 64) {
                queue.push_back(std::move(message));
            } else {
                p->ended = true;
            }
        }
    });
    peer->pc->onDataChannel([weak](const std::shared_ptr<rtc::DataChannel>& channel) {
        if (const auto session = weak.lock()) {
            bind_channel(session, channel);
        }
    });
}

void Server::run() {
    stream::ParameterSets parameters;
    bool awaiting_idr = true;
    stream::Resolution running = stream::kResolutions.back();
    auto next_settings_check = std::chrono::steady_clock::now();
    std::string settings_error;
    while (!quit_) {
        if (std::chrono::steady_clock::now() >= next_settings_check) {
            next_settings_check += 1s;
            try {
                const std::scoped_lock lock(mutex_);
                const auto configured = settings_.read().at("resolution").get<std::string>();
                resolution_ = stream::resolution(configured);
                if (!settings_error.empty() && error_ == settings_error) {
                    error_.clear();
                }
                settings_error.clear();
            } catch (const std::exception& error) {
                const std::scoped_lock lock(mutex_);
                settings_error = error_ = error.what();
            }
        }
        std::shared_ptr<Session> peer;
        stream::Resolution requested{};
        {
            const std::scoped_lock lock(mutex_);
            peer = peer_;
            requested = resolution_;
        }
        if (peer) {
            peer->protocol->tick();
        }
        if (peer && peer->ended) {
            peer->ended = true;
            peer->connected = false;
            peer->protocol->stop();
            std::vector<std::shared_ptr<rtc::DataChannel>> channels;
            {
                const std::scoped_lock lock(peer->mutex);
                channels = peer->channels;
            }
            for (const auto& channel : channels) {
                channel->close();
            }
            // Complete SCTP stream resets before closing the transport.
            // An immediate PC close sends ABORT to a still-live browser
            // during page reload and reports spurious HID channel errors.
            const auto deadline = std::chrono::steady_clock::now() + 250ms;
            while (std::chrono::steady_clock::now() < deadline &&
                   std::any_of(channels.begin(), channels.end(),
                       [](const auto& c) { return !c->isClosed(); })) {
                std::this_thread::sleep_for(5ms);
            }
            peer->pc->close();
            const std::scoped_lock lock(mutex_);
            if (peer_ == peer) {
                peer_.reset();
                ready_.notify_all();
            }
            peer.reset();
        }
        if (streaming_ && (!peer || !peer->connected || requested.width != running.width)) {
            capture_->stop();
            streaming_ = false;
            const std::scoped_lock lock(mutex_);
            frames_.clear();
        }
        if (peer && peer->connected && !streaming_) {
            try {
                parameters = {};
                awaiting_idr = true;
                resync_ = false;
                last_frame_ = now();
                const auto epoch =
                    static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count() *
                                          9 / 100);
                capture_->start(
                    [this, epoch, source_epoch = uint32_t{}, first = true](
                        stream::Bytes bytes, uint32_t timestamp) mutable {
                        // Keep RTP time continuous when an encoder restart resets its clock.
                        if (first) {
                            source_epoch = timestamp;
                            first = false;
                        }
                        last_frame_ = now();
                        ++captured_;
                        const std::scoped_lock lock(mutex_);
                        if (frames_.size() >= 3) {
                            dropped_ += frames_.size();
                            frames_.clear();
                            resync_ = true;
                        }
                        frames_.push_back({std::move(bytes), epoch + timestamp - source_epoch});
                        ready_.notify_one();
                    },
                    bitrate_, requested);
                running = requested;
                {
                    const std::scoped_lock lock(mutex_);
                    active_resolution_ = running;
                    streaming_ = true;
                }
            } catch (const std::exception& e) {
                {
                    const std::scoped_lock lock(mutex_);
                    error_ = e.what();
                }
                peer->ended = true;
                continue;
            }
        }
        if (streaming_ && peer) {
            if (now() - last_frame_ > 3) {
                {
                    const std::scoped_lock lock(mutex_);
                    error_ = "Hardware encoder stopped producing frames";
                }
                peer->ended = true;
                continue;
            }
            if (resync_.exchange(false)) {
                awaiting_idr = true;
                peer->idr = true;
            }
            if (peer->idr.exchange(false)) {
                capture_->keyframe();
            }
            Frame frame{};
            {
                std::unique_lock lock(mutex_);
                if (frames_.empty()) {
                    ready_.wait_for(lock, 20ms);
                }
                if (frames_.empty()) {
                    continue;
                }
                frame = std::move(frames_.front());
                frames_.pop_front();
            }
            parameters.observe(frame.bytes);
            const bool key = stream::is_idr(frame.bytes);
            if (!peer->track->isOpen() || (awaiting_idr && !key)) {
                ++dropped_;
                continue;
            }
            if (peer->track->bufferedAmount() > 512UL * 1024UL) {
                awaiting_idr = true;
                peer->idr = true;
                ++dropped_;
                continue;
            }
            if (key) {
                frame.bytes = parameters.with_parameter_sets(frame.bytes);
                awaiting_idr = false;
            }
            try {
                rtc::FrameInfo info(frame.timestamp);
                info.isKeyFrame = key;
                peer->track->sendFrame(frame.bytes.data(), frame.bytes.size(), info);
                ++sent_;
                bytes_ += frame.bytes.size();
            } catch (const std::exception& e) {
                std::cerr << "Video send: " << e.what() << '\n';
                peer->ended = true;
            }
        } else {
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, 20ms);
        }
    }
    capture_->stop();
    streaming_ = false;
    input_->release();
}

Server::Server(unsigned bitrate, const std::string& config_path)
    : capture_(stream::make_capture()),
      input_(std::make_shared<stream::Input>(stream::make_webos_input_device())), bitrate_(bitrate),
      config_path_(config_path), settings_(config_path),
      resolution_(stream::resolution(settings_.read().at("resolution").get<std::string>())),
      active_resolution_(resolution_) {
    worker_ = std::jthread([this] { run(); });
}

Server::~Server() {
    stop();
    worker_.join();
    if (peer_) {
        peer_->protocol->stop();
        peer_->pc->close();
    }
}

json Server::status() {
    const std::scoped_lock lock(mutex_);
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double cpu = static_cast<double>(usage.ru_utime.tv_sec) + usage.ru_utime.tv_usec / 1e6 +
                       static_cast<double>(usage.ru_stime.tv_sec) + usage.ru_stime.tv_usec / 1e6;
    const auto& video = streaming_ ? active_resolution_ : resolution_;
    return {{"application", "org.webosbrew.jetkvm"}, {"source", "Nyx hardware H.264"},
        {"streaming", static_cast<bool>(streaming_)}, {"resolution", resolution_.id},
        {"width", video.width}, {"height", video.height}, {"targetFps", video.fps},
        {"bitrateKbps", bitrate_}, {"captured", static_cast<uint64_t>(captured_)},
        {"sent", static_cast<uint64_t>(sent_)}, {"dropped", static_cast<uint64_t>(dropped_)},
        {"encodedBytes", static_cast<uint64_t>(bytes_)}, {"inputEvents", input_->events()},
        {"error", error_}, {"cpuSeconds", cpu},
        {"monotonicMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()}};
}

json Server::video_state() {
    const auto state = status();
    return {{"ready", state.at("error") == ""}, {"width", state.at("width")},
        {"height", state.at("height")}, {"fps", state.at("targetFps")},
        {"streaming", state.at("streaming").get<bool>() ? 1 : 0}};
}

std::shared_ptr<Session> Server::accept_offer(const std::string& encoded) {
    const auto request = json::parse(unbase64(encoded));
    if (request.at("type") != "offer") {
        throw std::invalid_argument("Expected an SDP offer");
    }
    rtc::Description offer(request.at("sdp").get<std::string>(), "offer");
    auto peer = std::make_shared<Session>();
    rtc::Configuration config{};
    config.disableAutoNegotiation = true;
    config.portRangeBegin = 50000;
    config.portRangeEnd = 50100;
    config.maxMessageSize = 128UL * 1024UL;
    peer->pc = std::make_shared<rtc::PeerConnection>(config);
    {
        std::unique_lock lock(mutex_);
        if (peer_ && peer_->ended) {
            ready_.wait_for(lock, 1s, [this] { return !peer_; });
        }
        if (peer_) {
            throw std::runtime_error("A viewer is already connected");
        }
        peer->protocol = std::make_shared<stream::JetKvmProtocol>(
            input_, config_path_, [this] { return video_state(); });
        peer_ = peer;
        error_.clear();
    }
    try {
        configure_signaling(peer);
        configure_video(peer, offer, bitrate_);
        peer->pc->setRemoteDescription(offer);
        peer->pc->setLocalDescription(rtc::Description::Type::Answer);
        return peer;
    } catch (...) {
        peer->ended = true;
        throw;
    }
}

void Server::signaling(httplib::ws::WebSocket& ws) {
    std::shared_ptr<Session> peer;
    try {
        ws.set_read_timeout(0, 50000);
        ws.send(json{{"type", "device-metadata"}, {"data", {{"deviceVersion", "0.5.9"}}}}.dump());
        std::deque<json> pending;
        while (!quit_ && ws.is_open() && (!peer || !peer->ended)) {
            if (peer) {
                std::deque<json> signals;
                {
                    const std::scoped_lock lock(peer->mutex);
                    signals.swap(peer->signals);
                }
                for (const auto& signal : signals) {
                    if (!ws.send(signal.dump())) {
                        throw std::runtime_error("Signaling write failed");
                    }
                }
            }
            std::string text;
            const auto result = ws.read(text);
            if (result == httplib::ws::Timeout) {
                continue;
            }
            if (result == httplib::ws::Fail) {
                break;
            }
            if (result != httplib::ws::Text || text.size() > 128UL * 1024UL) {
                throw std::invalid_argument("Invalid signaling frame");
            }
            if (text == "ping") {
                ws.send("pong");
                continue;
            }
            const auto message = json::parse(text);
            const auto type = message.at("type").get<std::string>();
            if (type == "offer") {
                if (peer) {
                    throw std::invalid_argument("Reconnect before replacing the offer");
                }
                peer = accept_offer(message.at("data").at("sd").get<std::string>());
                for (const auto& candidate : pending) {
                    add_candidate(peer, candidate);
                }
                pending.clear();
            } else if (type == "new-ice-candidate") {
                if (peer) {
                    add_candidate(peer, message.at("data"));
                } else {
                    if (pending.size() >= 64) {
                        throw std::invalid_argument("Too many ICE candidates");
                    }
                    pending.push_back(message.at("data"));
                }
            } else {
                throw std::invalid_argument("Unknown signaling message");
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "JetKVM signaling: " << e.what() << '\n';
        ws.close(
            httplib::ws::CloseStatus::PolicyViolation, "Session unavailable or invalid signaling");
    }
    if (peer) {
        peer->ended = true;
        ready_.notify_all();
    }
    ws.close();
}

void Server::add_candidate(const std::shared_ptr<Session>& peer, const json& value) {
    const auto candidate = value.at("candidate").get<std::string>();
    if (candidate.empty()) {
        return;
    }
    const auto mid = value.contains("sdpMid") && value.at("sdpMid").is_string()
                         ? value.at("sdpMid").get<std::string>()
                         : "0";
    peer->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
}
void Server::stop() noexcept {
    quit_ = true;
    worker_.request_stop();
    ready_.notify_all();
}
} // namespace stream
