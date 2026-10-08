#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/Json.hpp"
#include "core/ThreadPool.hpp"

namespace mss {

// An in-memory cache for media segment files, with a memory limit and
// "least recently used" (LRU) eviction.
//
// Why: many viewers usually watch the same popular segments. Reading them
// from memory is much faster than from disk, which keeps latency low.
//
// Features:
//  * LRU: when the memory budget is full, the segment that was used longest
//    ago is removed first.
//  * Request coalescing: if 10 clients ask for the same missing segment at the
//    same moment, the file is read from disk only once; the other 9 wait for
//    that read and share the result.
//  * Zero-copy sharing: cached data is a shared_ptr, so the HTTP server and
//    RTSP sessions use the same bytes without copying them.
//  * Prefetching: a segment can be loaded in the background before a player
//    asks for it, so it is already in memory when needed.
class SegmentCache {
public:
    using Data = std::shared_ptr<const std::string>;

    explicit SegmentCache(std::size_t capacityBytes);

    // Returns the file's bytes (from memory, or read from disk and cached).
    // Returns nullptr if the file cannot be read.
    Data get(const std::string& path);

    // Loads a file into the cache on a background thread (does nothing if it
    // is already cached or being loaded).
    void prefetch(ThreadPool& pool, const std::string& path);

    // Drops every cached file whose path starts with `prefix`.
    void invalidatePrefix(const std::string& prefix);

    Json statsJson() const;

private:
    struct Entry {
        Data data;
        std::list<std::string>::iterator lruPosition;
    };

    void insertLocked(const std::string& path, const Data& data);

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable loadFinished_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> lru_;               // front = most recently used
    std::unordered_set<std::string> loading_;  // files currently being read from disk
    std::size_t usedBytes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
    std::uint64_t coalesced_ = 0;  // requests that waited for another thread's disk read
};

// Reads a whole file into a string. Returns false if it cannot be read.
bool readWholeFile(const std::string& path, std::string& out);

}  // namespace mss
