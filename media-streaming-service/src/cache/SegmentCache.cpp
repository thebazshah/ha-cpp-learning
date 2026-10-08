#include "cache/SegmentCache.hpp"

#include <fstream>
#include <iterator>

#include "core/StringUtils.hpp"

namespace mss {

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

SegmentCache::SegmentCache(std::size_t capacityBytes) : capacity_(capacityBytes) {}

SegmentCache::Data SegmentCache::get(const std::string& path) {
    std::unique_lock<std::mutex> lock(mutex_);
    bool waitedForOtherThread = false;
    for (;;) {
        const auto it = entries_.find(path);
        if (it != entries_.end()) {
            // Cache hit: mark the entry as most recently used.
            lru_.splice(lru_.begin(), lru_, it->second.lruPosition);
            ++hits_;
            if (waitedForOtherThread) ++coalesced_;
            return it->second.data;
        }
        if (loading_.count(path) == 0) break;  // nobody is loading it: we will
        // Another thread is reading this file right now. Wait for it instead
        // of reading the same file a second time.
        waitedForOtherThread = true;
        loadFinished_.wait(lock);
    }

    ++misses_;
    loading_.insert(path);
    lock.unlock();

    // Read from disk without holding the lock, so other requests are not blocked.
    auto content = std::make_shared<std::string>();
    const bool ok = readWholeFile(path, *content);

    lock.lock();
    loading_.erase(path);
    Data data;
    if (ok) {
        data = content;
        insertLocked(path, data);
    }
    lock.unlock();
    loadFinished_.notify_all();
    return data;
}

void SegmentCache::insertLocked(const std::string& path, const Data& data) {
    // Files bigger than a quarter of the budget would push out too much else.
    if (capacity_ == 0 || data->size() > capacity_ / 4) return;

    lru_.push_front(path);
    entries_[path] = Entry{data, lru_.begin()};
    usedBytes_ += data->size();

    // Evict the least recently used files until we are within budget.
    while (usedBytes_ > capacity_ && !lru_.empty()) {
        const std::string victim = lru_.back();
        lru_.pop_back();
        const auto it = entries_.find(victim);
        if (it != entries_.end()) {
            usedBytes_ -= it->second.data->size();
            entries_.erase(it);
            ++evictions_;
        }
    }
}

void SegmentCache::prefetch(ThreadPool& pool, const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.count(path) > 0 || loading_.count(path) > 0) return;
    }
    pool.submit([this, path] { get(path); });
}

void SegmentCache::invalidatePrefix(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (str::startsWith(it->first, prefix)) {
            usedBytes_ -= it->second.data->size();
            lru_.erase(it->second.lruPosition);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

Json SegmentCache::statsJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t lookups = hits_ + misses_;
    Json stats = Json::object();
    stats.set("capacityBytes", capacity_)
        .set("usedBytes", usedBytes_)
        .set("entries", entries_.size())
        .set("hits", hits_)
        .set("misses", misses_)
        .set("hitRatio", lookups == 0 ? 0.0 : static_cast<double>(hits_) / static_cast<double>(lookups))
        .set("evictions", evictions_)
        .set("coalescedRequests", coalesced_);
    return stats;
}

}  // namespace mss
