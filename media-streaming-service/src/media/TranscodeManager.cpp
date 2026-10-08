#include "media/TranscodeManager.hpp"

#include <signal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Logger.hpp"
#include "core/StringUtils.hpp"
#include "media/H264.hpp"
#include "media/TsDemuxer.hpp"

namespace mss {
namespace fs = std::filesystem;

namespace {

// Changing the encoding recipe must produce new cache folders, otherwise old
// results would be reused. Bump this number whenever buildCommand() changes.
constexpr const char* kRecipeVersion = "v1";

std::string lastLines(const std::string& path, std::size_t maxLines) {
    std::string content;
    if (!readWholeFile(path, content)) return "";
    std::vector<std::string> lines;
    for (std::string& line : str::split(content, '\n')) {
        line = str::trim(line);
        if (!line.empty()) lines.push_back(line);
    }
    if (lines.size() > maxLines) lines.erase(lines.begin(), lines.end() - static_cast<long>(maxLines));
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) out += " | ";
        out += line;
    }
    return out;
}

}  // namespace

const char* jobStateName(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Completed: return "completed";
        case JobState::Failed: return "failed";
    }
    return "unknown";
}

// =================================================================== TranscodeJob

TranscodeJob::TranscodeJob(MediaItem media, std::vector<Rendition> renditions, std::string outputDir)
    : media_(std::move(media)),
      renditions_(std::move(renditions)),
      outputDir_(std::move(outputDir)),
      streamInfo_(renditions_.size()),
      createdAt_(std::chrono::steady_clock::now()) {}

std::string TranscodeJob::renditionDir(std::size_t index) const {
    return outputDir_ + "/" + renditions_.at(index).name;
}

std::string TranscodeJob::playlistPath(std::size_t index) const { return renditionDir(index) + "/index.m3u8"; }

JobState TranscodeJob::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

double TranscodeJob::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

std::string TranscodeJob::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool TranscodeJob::ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

double TranscodeJob::secondsSinceFinished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != JobState::Completed && state_ != JobState::Failed) return 0.0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - finishedAt_).count();
}

bool TranscodeJob::waitUntilReady(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, timeout, [this] { return ready_ || state_ == JobState::Failed; });
    return ready_;
}

bool TranscodeJob::readPlaylist(std::size_t index, HlsMediaPlaylist& out) const {
    if (index >= renditions_.size()) return false;
    std::string text;
    if (!readWholeFile(playlistPath(index), text)) return false;
    return parseMediaPlaylist(text, out);
}

bool TranscodeJob::streamInfo(std::size_t index, SegmentCache& cache, RenditionStreamInfo& out) {
    if (index >= renditions_.size()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streamInfo_[index]) {
            out = *streamInfo_[index];
            return true;
        }
    }

    // Read segment 0 and look inside it with our own parsers.
    HlsMediaPlaylist playlist;
    if (!readPlaylist(index, playlist) || playlist.segments.empty()) return false;
    const SegmentCache::Data segment = cache.get(renditionDir(index) + "/" + playlist.segments[0].uri);
    if (!segment) return false;

    DemuxResult demuxed;
    std::string error;
    if (!demuxTransportStream(reinterpret_cast<const std::uint8_t*>(segment->data()), segment->size(), demuxed,
                              error)) {
        LOG_WARN("transcode") << "Cannot read first segment of " << media_.id() << ": " << error;
        return false;
    }

    RenditionStreamInfo info;
    std::vector<std::string> codecs;
    if (demuxed.hasVideo && !demuxed.sps.empty()) {
        h264::SpsInfo sps;
        if (h264::parseSps(demuxed.sps.data(), demuxed.sps.size(), sps)) {
            info.hasVideo = true;
            info.width = sps.width;
            info.height = sps.height;
            info.profileLevelId = h264::profileLevelId(sps);
            info.sps = demuxed.sps;
            info.pps = demuxed.pps;
            codecs.push_back(h264::codecString(sps));
        }
    }
    if (demuxed.hasAudio && demuxed.audioConfigKnown) {
        info.hasAudio = true;
        info.audioConfigKnown = true;
        info.audioConfig = demuxed.audioConfig;
        codecs.push_back("mp4a.40." + std::to_string(demuxed.audioConfig.audioObjectType));
    }
    if (!info.hasVideo && !info.hasAudio) return false;

    info.firstPts = demuxed.frames.empty() ? 0 : demuxed.frames.front().dts;
    for (const MediaFrame& frame : demuxed.frames) info.firstPts = std::min(info.firstPts, frame.pts);
    for (std::size_t i = 0; i < codecs.size(); ++i) info.codecs += (i > 0 ? "," : "") + codecs[i];

    std::lock_guard<std::mutex> lock(mutex_);
    streamInfo_[index] = info;
    out = info;
    return true;
}

Json TranscodeJob::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    if (state_ == JobState::Running) elapsed = std::chrono::duration<double>(now - startedAt_).count();
    if (state_ == JobState::Completed || state_ == JobState::Failed) {
        elapsed = std::chrono::duration<double>(finishedAt_ - startedAt_).count();
    }

    Json renditions = Json::array();
    for (std::size_t i = 0; i < renditions_.size(); ++i) {
        const Rendition& rendition = renditions_[i];
        Json item = Json::object();
        item.set("index", i)
            .set("name", rendition.name)
            .set("label", rendition.label())
            .set("videoKbps", rendition.videoKbps)
            .set("audioKbps", rendition.audioKbps)
            .set("bandwidth", rendition.peakBandwidth());
        if (streamInfo_[i]) {
            item.set("width", streamInfo_[i]->width).set("height", streamInfo_[i]->height);
            item.set("codecs", streamInfo_[i]->codecs);
        }
        renditions.push(item);
    }

    Json job = Json::object();
    job.set("media", media_.id())
        .set("state", jobStateName(state_))
        .set("progress", progress_)
        .set("ready", ready_)
        .set("error", error_)
        .set("elapsedSeconds", elapsed)
        .set("renditions", renditions);
    return job;
}

void TranscodeJob::markRunning(std::shared_ptr<Process> process) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = JobState::Running;
    startedAt_ = std::chrono::steady_clock::now();
    process_ = std::move(process);
}

void TranscodeJob::updateProgress(double progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_ = std::max(progress_, std::min(1.0, std::max(0.0, progress)));
}

void TranscodeJob::markReady() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_) return;
        ready_ = true;
    }
    changed_.notify_all();
}

void TranscodeJob::markCompleted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = JobState::Completed;
        progress_ = 1.0;
        ready_ = true;
        finishedAt_ = std::chrono::steady_clock::now();
        if (startedAt_ == std::chrono::steady_clock::time_point()) startedAt_ = finishedAt_;
        process_.reset();
    }
    changed_.notify_all();
}

void TranscodeJob::markFailed(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = JobState::Failed;
        error_ = error;
        finishedAt_ = std::chrono::steady_clock::now();
        if (startedAt_ == std::chrono::steady_clock::time_point()) startedAt_ = finishedAt_;
        process_.reset();
    }
    changed_.notify_all();
}

void TranscodeJob::wakeWaiters() const { changed_.notify_all(); }

// =================================================================== TranscodeManager

TranscodeManager::TranscodeManager(TranscodeSettings settings, SegmentCache& cache)
    : settings_(std::move(settings)), cache_(cache) {
    std::error_code ignored;
    fs::create_directories(settings_.cacheDir + "/hls", ignored);
}

TranscodeManager::~TranscodeManager() { shutdown(); }

std::string TranscodeManager::detectVideoEncoder(const std::string& ffmpegPath, const std::string& preferred) {
    const ProcessResult result = Process::run({ffmpegPath, "-hide_banner", "-encoders"}, 15);
    if (!result.started) return "";
    auto available = [&result](const std::string& name) {
        return result.output.find(" " + name + " ") != std::string::npos;
    };
    if (preferred != "auto" && !preferred.empty()) return available(preferred) ? preferred : "";
    for (const char* candidate : {"libx264", "h264_videotoolbox", "libopenh264"}) {
        if (available(candidate)) return candidate;
    }
    return "";
}

std::string TranscodeManager::jobKey(const MediaItem& item) const {
    // Anything that changes the output must be part of the key: the file
    // itself (path, size, modification time) and the encoding settings.
    std::ostringstream key;
    key << item.path << '|' << item.sizeBytes << '|' << item.modifiedUnix << '|' << kRecipeVersion << '|'
        << settings_.segmentSeconds << '|' << settings_.maxRenditions << '|' << settings_.videoEncoder << '|'
        << settings_.x264Preset;
    return key.str();
}

std::string TranscodeManager::outputDirFor(const MediaItem& item) const {
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(str::fnv1a64(jobKey(item))));
    return settings_.cacheDir + "/hls/" + mediaKindFolder(item.kind) + "-" + str::sanitizeForFileName(item.name, 40) +
           "-" + hash;
}

std::shared_ptr<TranscodeJob> TranscodeManager::createJob(const MediaItem& item) {
    const bool wantVideo = item.kind == MediaKind::Video;
    std::vector<Rendition> ladder = buildLadder(item.info, wantVideo, settings_.maxRenditions);
    return std::make_shared<TranscodeJob>(item, std::move(ladder), outputDirFor(item));
}

bool TranscodeManager::loadFinishedJobFromDisk(TranscodeJob& job) const {
    std::error_code ignored;
    if (!fs::exists(job.outputDir() + "/complete.json", ignored)) return false;
    for (std::size_t i = 0; i < job.renditions().size(); ++i) {
        HlsMediaPlaylist playlist;
        if (!job.readPlaylist(i, playlist) || !playlist.endList || playlist.segments.empty()) return false;
    }
    job.markCompleted();
    return true;
}

std::shared_ptr<TranscodeJob> TranscodeManager::findJob(const MediaItem& item) {
    const std::string key = jobKey(item);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(key);
    if (it != jobs_.end()) return it->second;
    if (!item.info.ok) return nullptr;

    // Maybe it was finished during an earlier run of the server.
    std::shared_ptr<TranscodeJob> job = createJob(item);
    if (job->renditions().empty() || !loadFinishedJobFromDisk(*job)) return nullptr;
    jobs_[key] = job;
    return job;
}

std::shared_ptr<TranscodeJob> TranscodeManager::ensureJob(const MediaItem& item) {
    const std::string key = jobKey(item);
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = jobs_.find(key);
    if (it != jobs_.end()) {
        // A failed job is retried after 30 seconds (maybe the problem was temporary).
        const bool canRetry = it->second->state() == JobState::Failed && !shuttingDown_ &&
                              it->second->secondsSinceFinished() > 30.0;
        if (!canRetry) return it->second;
        jobs_.erase(it);
    }

    std::shared_ptr<TranscodeJob> job = createJob(item);
    jobs_[key] = job;

    if (!item.info.ok) {
        job->markFailed("cannot read media file: " + item.info.error);
        return job;
    }
    if (job->renditions().empty()) {
        job->markFailed("the file has no audio or video stream that can be streamed");
        return job;
    }
    if (shuttingDown_) {
        job->markFailed("server is shutting down");
        return job;
    }
    if (loadFinishedJobFromDisk(*job)) {
        LOG_INFO("transcode") << "Using cached HLS output for " << item.id();
        return job;
    }

    // Start from a clean folder (an older run may have been interrupted).
    std::error_code ignored;
    fs::remove_all(job->outputDir(), ignored);
    cache_.invalidatePrefix(job->outputDir() + "/");
    queue_.push_back(job);
    LOG_INFO("transcode") << "Queued " << item.id() << " (" << job->renditions().size() << " renditions)";
    startQueuedJobsLocked();
    return job;
}

void TranscodeManager::startQueuedJobsLocked() {
    // Join worker threads that have already finished, so they do not pile up.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->done->load()) {
            if (it->thread.joinable()) it->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }

    while (!shuttingDown_ && running_ < settings_.maxConcurrentJobs && !queue_.empty()) {
        std::shared_ptr<TranscodeJob> job = queue_.front();
        queue_.pop_front();
        ++running_;
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers_.push_back(Worker{std::thread([this, job, done] {
                                      runJob(job);
                                      {
                                          std::lock_guard<std::mutex> lock(mutex_);
                                          --running_;
                                          startQueuedJobsLocked();
                                      }
                                      done->store(true);
                                  }),
                                  done});
    }
}

std::vector<std::string> TranscodeManager::buildCommand(const TranscodeJob& job) const {
    const MediaItem& media = job.media();
    const std::vector<Rendition>& renditions = job.renditions();
    const bool video = !renditions.empty() && renditions[0].hasVideo();
    const bool audio = media.info.hasAudio;
    const std::string segment = std::to_string(settings_.segmentSeconds);

    std::vector<std::string> args = {
        settings_.ffmpegPath, "-hide_banner", "-nostdin", "-y", "-loglevel", "warning",
        // Machine-readable progress lines ("out_time_us=...") on stdout.
        "-progress", "pipe:1", "-nostats",
        "-i", "file:" + media.path,
    };

    if (video) {
        // One decoder feeds every rendition: split the picture into N copies
        // and scale each copy. The scale expression keeps the aspect ratio and
        // works for both landscape and portrait videos:
        //   landscape -> height = N, width = auto (even)
        //   portrait  -> width  = N, height = auto (even)
        std::string graph = "[0:v:0]split=" + std::to_string(renditions.size());
        for (std::size_t i = 0; i < renditions.size(); ++i) graph += "[s" + std::to_string(i) + "]";
        for (std::size_t i = 0; i < renditions.size(); ++i) {
            const std::string side = std::to_string(renditions[i].shortSide);
            graph += ";[s" + std::to_string(i) + "]scale=w='if(gt(iw,ih),-2," + side + ")':h='if(gt(iw,ih)," + side +
                     ",-2)',format=yuv420p[v" + std::to_string(i) + "]";
        }
        args.insert(args.end(), {"-filter_complex", graph});
    }

    // A key frame every `segmentSeconds` seconds in every rendition, at exactly
    // the same moments. This makes segment boundaries line up across
    // renditions, so players can switch quality at any segment boundary.
    const double frameRate = media.info.frameRate > 1 ? media.info.frameRate : 30.0;
    const std::string gop = std::to_string(std::max(1L, std::lround(frameRate * settings_.segmentSeconds)));

    for (std::size_t i = 0; i < renditions.size(); ++i) {
        const Rendition& rendition = renditions[i];
        const std::string dir = job.renditionDir(i);

        if (video) args.insert(args.end(), {"-map", "[v" + std::to_string(i) + "]"});
        if (audio) args.insert(args.end(), {"-map", "0:a:0"});

        if (video) {
            args.insert(args.end(), {"-c:v", settings_.videoEncoder});
            if (settings_.videoEncoder == "libx264") {
                // No B-frames: lower decoding delay and simpler RTP timing.
                // sc_threshold 0: no extra key frames on scene cuts (keeps renditions aligned).
                args.insert(args.end(), {"-preset", settings_.x264Preset, "-profile:v", "main", "-bf", "0",
                                         "-sc_threshold", "0"});
            } else {
                args.insert(args.end(), {"-profile:v", "main"});
            }
            args.insert(args.end(), {"-b:v", std::to_string(rendition.videoKbps) + "k",
                                     "-maxrate", std::to_string(rendition.maxRateKbps()) + "k",
                                     "-bufsize", std::to_string(rendition.bufferKbps()) + "k",
                                     "-g", gop, "-keyint_min", gop,
                                     "-force_key_frames", "expr:gte(t,n_forced*" + segment + ")"});
        }
        if (audio) {
            // Same audio format in every rendition (48 kHz stereo AAC-LC), so
            // switching renditions never changes the audio configuration.
            args.insert(args.end(), {"-c:a", "aac", "-b:a", std::to_string(rendition.audioKbps) + "k", "-ac", "2",
                                     "-ar", "48000"});
        }
        args.insert(args.end(), {
            "-max_muxing_queue_size", "4096",
            "-f", "hls",
            "-hls_time", segment,
            "-hls_playlist_type", "event",  // the playlist grows while we encode
            "-hls_list_size", "0",          // keep every segment in the playlist
            // temp_file: segments appear under their final name only when complete.
            "-hls_flags", "independent_segments+temp_file",
            "-hls_segment_type", "mpegts",
            "-hls_segment_filename", dir + "/seg_%05d.ts",
            dir + "/index.m3u8",
        });
    }
    return args;
}

void TranscodeManager::checkReadiness(TranscodeJob& job) const {
    if (job.ready()) return;
    for (std::size_t i = 0; i < job.renditions().size(); ++i) {
        HlsMediaPlaylist playlist;
        if (!job.readPlaylist(i, playlist) || playlist.segments.empty()) return;
    }
    job.markReady();
    LOG_INFO("transcode") << job.media().id() << " is ready to stream (first segments are available)";
}

void TranscodeManager::runJob(const std::shared_ptr<TranscodeJob>& job) {
    const MediaItem& media = job->media();
    std::error_code ignored;
    for (std::size_t i = 0; i < job->renditions().size(); ++i) fs::create_directories(job->renditionDir(i), ignored);

    const std::vector<std::string> command = buildCommand(*job);
    std::string commandText;
    for (const std::string& part : command) commandText += (commandText.empty() ? "" : " ") + part;
    LOG_DEBUG("transcode") << "Running: " << commandText;

    const std::string logPath = job->outputDir() + "/ffmpeg.log";
    std::string error;
    std::shared_ptr<Process> process = Process::start(command, logPath, error);
    if (!process) {
        LOG_ERROR("transcode") << "Cannot start ffmpeg for " << media.id() << ": " << error;
        job->markFailed("cannot start ffmpeg: " + error);
        return;
    }
    job->markRunning(process);
    LOG_INFO("transcode") << "Transcoding " << media.id() << " into " << job->renditions().size()
                          << " renditions (ffmpeg pid " << process->pid() << ")";

    // ffmpeg prints progress blocks like:  out_time_us=4000000 ... progress=continue
    const double duration = media.info.durationSeconds;
    std::string line;
    while (process->readLine(line)) {
        if (str::startsWith(line, "out_time_us=")) {
            long long micros = 0;
            if (duration > 0 && str::parseInt64(line.substr(12), micros)) {
                job->updateProgress(static_cast<double>(micros) / 1e6 / duration);
            }
        } else if (str::startsWith(line, "progress=")) {
            checkReadiness(*job);  // once per progress block (about twice per second)
        }
    }
    const int exitCode = process->wait();

    bool stopping;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping = shuttingDown_;
    }
    if (stopping) {
        job->markFailed("stopped because the server is shutting down");
        return;
    }

    bool complete = exitCode == 0;
    for (std::size_t i = 0; complete && i < job->renditions().size(); ++i) {
        HlsMediaPlaylist playlist;
        complete = job->readPlaylist(i, playlist) && playlist.endList && !playlist.segments.empty();
    }
    if (!complete) {
        const std::string details = lastLines(logPath, 3);
        LOG_ERROR("transcode") << "ffmpeg failed for " << media.id() << " (exit code " << exitCode << "): " << details;
        job->markFailed("ffmpeg failed (exit code " + std::to_string(exitCode) + ")" +
                        (details.empty() ? "" : ": " + details));
        return;
    }

    // Mark the folder as finished so later server runs can reuse it.
    std::ofstream marker(job->outputDir() + "/complete.json");
    marker << job->toJson().dump() << "\n";
    marker.close();
    job->markCompleted();
    LOG_INFO("transcode") << "Finished " << media.id();
}

void TranscodeManager::shutdown() {
    std::vector<Worker> workers;
    std::vector<std::shared_ptr<TranscodeJob>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_ && workers_.empty()) return;
        shuttingDown_ = true;
        for (auto& entry : jobs_) jobs.push_back(entry.second);
        for (const auto& job : queue_) job->markFailed("server is shutting down");
        queue_.clear();
    }

    // Ask every running ffmpeg to stop. ffmpeg reacts to SIGTERM by closing
    // its files and exiting within a moment.
    for (const auto& job : jobs) {
        std::shared_ptr<Process> process;
        {
            std::lock_guard<std::mutex> lock(job->mutex_);
            process = job->process_;
        }
        if (process) process->sendSignal(SIGTERM);
        job->wakeWaiters();
    }

    // If something does not stop within 5 seconds, force it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        bool allDone = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const Worker& worker : workers_) allDone = allDone && worker.done->load();
        }
        if (allDone) break;
        if (std::chrono::steady_clock::now() > deadline) {
            for (const auto& job : jobs) {
                std::shared_ptr<Process> process;
                {
                    std::lock_guard<std::mutex> lock(job->mutex_);
                    process = job->process_;
                }
                if (process) process->sendSignal(SIGKILL);
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers.swap(workers_);
    }
    for (Worker& worker : workers) {
        if (worker.thread.joinable()) worker.thread.join();
    }
}

Json TranscodeManager::statsJson() const {
    std::vector<std::shared_ptr<TranscodeJob>> jobs;
    Json stats = Json::object();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : jobs_) jobs.push_back(entry.second);
        stats.set("running", running_)
            .set("queued", queue_.size())
            .set("maxConcurrent", settings_.maxConcurrentJobs)
            .set("videoEncoder", settings_.videoEncoder)
            .set("segmentSeconds", settings_.segmentSeconds);
    }
    Json list = Json::array();
    for (const auto& job : jobs) list.push(job->toJson());
    stats.set("jobs", list);
    return stats;
}

}  // namespace mss
