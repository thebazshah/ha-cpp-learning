#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cache/SegmentCache.hpp"
#include "core/Json.hpp"
#include "core/Process.hpp"
#include "media/Aac.hpp"
#include "media/BitrateLadder.hpp"
#include "media/HlsPlaylist.hpp"
#include "media/MediaLibrary.hpp"

namespace mss {

enum class JobState { Queued, Running, Completed, Failed };
const char* jobStateName(JobState state);

// Codec details of one rendition, learned by reading its first segment with
// our own MPEG-TS / H.264 / AAC parsers. Used for the HLS master playlist
// (CODECS, RESOLUTION) and for the RTSP session description (SDP).
struct RenditionStreamInfo {
    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;
    int height = 0;
    std::string codecs;               // e.g. "avc1.4d401f,mp4a.40.2"
    std::string profileLevelId;       // e.g. "4D401F" (RTSP)
    std::vector<std::uint8_t> sps;    // H.264 parameter sets (RTSP sprop-parameter-sets)
    std::vector<std::uint8_t> pps;
    bool audioConfigKnown = false;
    aac::AdtsHeader audioConfig;      // sample rate / channels (RTSP config=)
    std::int64_t firstPts = 0;        // earliest timestamp of segment 0 (90 kHz) = media time 0
};

// One transcoding job: turns one media file into an HLS bitrate ladder.
//
// Files on disk (inside the cache folder):
//   cache/hls/<job-folder>/r0/index.m3u8, r0/seg_00000.ts, ...   (lowest quality)
//   cache/hls/<job-folder>/r1/...
//   cache/hls/<job-folder>/ffmpeg.log                             (ffmpeg's messages)
//   cache/hls/<job-folder>/complete.json                          (written when finished)
//
// Playback can start while ffmpeg is still working: a job is "ready" as soon
// as every rendition has its first segment.
class TranscodeJob {
public:
    TranscodeJob(MediaItem media, std::vector<Rendition> renditions, std::string outputDir);

    const MediaItem& media() const { return media_; }
    const std::vector<Rendition>& renditions() const { return renditions_; }
    const std::string& outputDir() const { return outputDir_; }
    std::string renditionDir(std::size_t index) const;
    std::string playlistPath(std::size_t index) const;

    JobState state() const;
    double progress() const;  // 0.0 ... 1.0
    std::string error() const;
    bool ready() const;
    double secondsSinceFinished() const;  // 0 while the job is still queued or running

    // Blocks until the job is ready to play (or has failed), at most `timeout`.
    bool waitUntilReady(std::chrono::milliseconds timeout) const;

    // Reads the current media playlist of one rendition from disk.
    bool readPlaylist(std::size_t index, HlsMediaPlaylist& out) const;

    // Returns codec details of a rendition (computed once from segment 0, then cached).
    bool streamInfo(std::size_t index, SegmentCache& cache, RenditionStreamInfo& out);

    Json toJson() const;

private:
    friend class TranscodeManager;

    void markRunning(std::shared_ptr<Process> process);
    void updateProgress(double progress);
    void markReady();
    void markCompleted();
    void markFailed(const std::string& error);
    void wakeWaiters() const;

    MediaItem media_;
    std::vector<Rendition> renditions_;
    std::string outputDir_;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    JobState state_ = JobState::Queued;
    double progress_ = 0.0;
    bool ready_ = false;
    std::string error_;
    std::vector<std::optional<RenditionStreamInfo>> streamInfo_;
    std::chrono::steady_clock::time_point createdAt_;
    std::chrono::steady_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point finishedAt_;
    std::shared_ptr<Process> process_;  // the running ffmpeg (only while Running)
};

struct TranscodeSettings {
    std::string ffmpegPath = "ffmpeg";
    std::string cacheDir;
    std::string videoEncoder = "libx264";
    std::string x264Preset = "veryfast";
    int segmentSeconds = 2;
    int maxRenditions = 4;
    int maxConcurrentJobs = 2;
};

// Starts ffmpeg jobs on demand, limits how many run at once, and remembers
// finished jobs so each file is transcoded only once (the results stay in
// the cache folder, even across server restarts).
class TranscodeManager {
public:
    TranscodeManager(TranscodeSettings settings, SegmentCache& cache);
    ~TranscodeManager();

    TranscodeManager(const TranscodeManager&) = delete;
    TranscodeManager& operator=(const TranscodeManager&) = delete;

    // Returns the job for this file, creating and queueing it if needed.
    std::shared_ptr<TranscodeJob> ensureJob(const MediaItem& item);

    // Returns the job only if it already exists (in memory or finished on disk).
    std::shared_ptr<TranscodeJob> findJob(const MediaItem& item);

    // Stops all running ffmpeg processes and waits for the job threads.
    void shutdown();

    Json statsJson() const;
    const TranscodeSettings& settings() const { return settings_; }

    // Picks a working H.264 encoder: the preferred one if ffmpeg has it,
    // otherwise libx264, then h264_videotoolbox (macOS hardware encoder).
    static std::string detectVideoEncoder(const std::string& ffmpegPath, const std::string& preferred);

    // Builds the ffmpeg command line for a job (public so tests can inspect it).
    std::vector<std::string> buildCommand(const TranscodeJob& job) const;

private:
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::string jobKey(const MediaItem& item) const;
    std::string outputDirFor(const MediaItem& item) const;
    std::shared_ptr<TranscodeJob> createJob(const MediaItem& item);
    bool loadFinishedJobFromDisk(TranscodeJob& job) const;
    void startQueuedJobsLocked();
    void runJob(const std::shared_ptr<TranscodeJob>& job);
    void checkReadiness(TranscodeJob& job) const;

    TranscodeSettings settings_;
    SegmentCache& cache_;

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<TranscodeJob>> jobs_;  // job key -> job
    std::deque<std::shared_ptr<TranscodeJob>> queue_;            // waiting for a free slot
    std::vector<Worker> workers_;
    int running_ = 0;
    bool shuttingDown_ = false;
};

}  // namespace mss
