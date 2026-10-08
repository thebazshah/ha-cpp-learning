#include "engine/Viewer.hpp"

#include "engine/GameSession.hpp"

namespace cge {
namespace {
constexpr std::size_t kMaxQueuedTexts = 64;  // state messages are small; drop the oldest beyond this
}  // namespace

Viewer::Viewer(std::uint64_t id, std::unique_ptr<ws::Connection> connection, std::string remoteIp)
    : id_(id), connection_(std::move(connection)), remoteIp_(std::move(remoteIp)) {}

Viewer::~Viewer() {
    close();
    join();
}

void Viewer::start(const std::shared_ptr<GameSession>& session, const std::shared_ptr<Viewer>& self) {
    std::weak_ptr<GameSession> weakSession = session;
    std::weak_ptr<Viewer> weakSelf = self;
    reader_ = std::thread([this, weakSession, weakSelf] { readerLoop(weakSession, weakSelf); });
    writer_ = std::thread([this] { writerLoop(); });
}

void Viewer::close() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        closing_ = true;
        queue_.clear();
        queuedFrames_ = 0;
    }
    queueChanged_.notify_all();
    connection_->close();  // also wakes up the reader thread
}

void Viewer::join() {
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) writer_.join();
}

std::size_t Viewer::queuedFrames() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return queuedFrames_;
}

bool Viewer::queueFrame(std::shared_ptr<const std::vector<std::uint8_t>> frame) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (closing_) return true;  // nothing to do; not a "drop"
        if (queuedFrames_ >= kMaxQueuedFrames) {
            ++framesDropped;
            return false;
        }
        Outgoing item;
        item.kind = Outgoing::Frame;
        item.frame = std::move(frame);
        queue_.push_back(std::move(item));
        ++queuedFrames_;
    }
    queueChanged_.notify_one();
    return true;
}

void Viewer::push(Outgoing item) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (closing_) return;
        if (item.kind == Outgoing::Text) {
            std::size_t texts = 0;
            for (const Outgoing& queued : queue_) texts += queued.kind == Outgoing::Text ? 1 : 0;
            if (texts >= kMaxQueuedTexts) {
                for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                    if (it->kind == Outgoing::Text) {
                        queue_.erase(it);
                        break;
                    }
                }
            }
        }
        queue_.push_back(std::move(item));
    }
    queueChanged_.notify_one();
}

void Viewer::queueText(std::string text) {
    Outgoing item;
    item.kind = Outgoing::Text;
    item.text = std::move(text);
    push(std::move(item));
}

void Viewer::queueBinary(std::vector<std::uint8_t> data) {
    Outgoing item;
    item.kind = Outgoing::Binary;
    item.binary = std::move(data);
    push(std::move(item));
}

void Viewer::readerLoop(std::weak_ptr<GameSession> weakSession, std::weak_ptr<Viewer> weakSelf) {
    ws::Message message;
    while (connection_->receive(message)) {
        std::shared_ptr<GameSession> session = weakSession.lock();
        std::shared_ptr<Viewer> self = weakSelf.lock();
        if (!session || !self) break;
        session->onViewerMessage(self, message);
    }
    // The client went away (or we closed the connection).
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        closing_ = true;
    }
    queueChanged_.notify_all();
    std::shared_ptr<GameSession> session = weakSession.lock();
    std::shared_ptr<Viewer> self = weakSelf.lock();
    if (session && self) session->onViewerClosed(self);
}

void Viewer::writerLoop() {
    for (;;) {
        Outgoing item;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueChanged_.wait(lock, [this] { return closing_ || !queue_.empty(); });
            if (closing_) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            if (item.kind == Outgoing::Frame) --queuedFrames_;
        }

        bool ok = true;
        switch (item.kind) {
            case Outgoing::Frame:
                ok = connection_->sendBinary(item.frame->data(), item.frame->size());
                if (ok) {
                    ++framesSent;
                    if (item.frame->size() > 1 && ((*item.frame)[1] & 0x01)) ++keyframesSent;
                }
                break;
            case Outgoing::Text:
                ok = connection_->sendText(item.text);
                break;
            case Outgoing::Binary:
                ok = connection_->sendBinary(item.binary.data(), item.binary.size());
                break;
        }
        if (!ok) {
            // Send failed or timed out: the client is gone or hopelessly slow.
            connection_->close();
            return;
        }
    }
}

}  // namespace cge
