// Media Streaming Service - web frontend (Alpine.js).
//
// What this page does:
//  1. Lists the files in the videos/ and audios/ folders (GET /api/media).
//  2. When you pick a file it asks the server to prepare the adaptive stream
//     (POST /api/media/{type}/{name}/prepare) and shows the transcoding progress.
//  3. Plays the HLS master playlist with hls.js (or natively in Safari).
//     hls.js measures the download speed and switches between the quality
//     levels by itself - that is client-side adaptive bitrate streaming.
//  4. Survives network interruptions: failed downloads are retried with an
//     increasing delay, decoder errors are recovered, and playback resumes
//     when the browser comes back online.
//  5. Shows live statistics of the player and of the server (GET /api/stats).

// The hls.js player object is kept OUTSIDE of Alpine's reactive data on
// purpose: Alpine wraps reactive objects in proxies, which would break hls.js.
let hls = null;
let playToken = 0;        // increases on every new playback; stale async work checks it and stops
let reconnectTimer = null;
let eventCounter = 0;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function emptyPlayerStats() {
  return {
    engine: '', position: 0, duration: 0, bufferAhead: 0,
    levelLabel: '', levelBitrate: 0, bandwidth: 0, lastSegment: '–',
    startupMs: 0, fragments: 0, levelSwitches: 0, stalls: 0,
    recoveredErrors: 0, reconnects: 0, droppedFrames: 0, totalFrames: 0,
  };
}

document.addEventListener('alpine:init', () => {
  Alpine.data('streamingApp', () => ({
    // ---------------------------------------------------------------- state
    serverOnline: true,
    serverStats: null,
    config: { rtspPort: 8554, segmentSeconds: 2 },
    tab: 'videos',
    library: { videos: [], audios: [] },
    libraryLoaded: false,
    loadingLibrary: false,
    selected: null,
    mode: 'hls',
    levels: [],
    selectedLevel: -1,
    status: { kind: 'idle', text: '' },
    prepareProgress: 0,
    playerStats: emptyPlayerStats(),
    events: [],
    copiedText: '',
    startRequestedAt: 0,
    hasStartedPlaying: false,
    manifestLoaded: false,
    reconnectAttempts: 0,

    // ---------------------------------------------------------------- computed values
    get currentList() {
      return this.library[this.tab] || [];
    },

    get rtspUrl() {
      if (!this.selected) return '';
      return `rtsp://${location.hostname}:${this.config.rtspPort}${this.selected.urls.rtspPath}`;
    },

    // ---------------------------------------------------------------- start-up
    async init() {
      await this.loadConfig();
      await this.loadLibrary();
      this.refreshServerStats();
      setInterval(() => this.refreshServerStats(), 2000);
      setInterval(() => this.updatePlayerStats(), 1000);
      window.addEventListener('online', () => this.onBrowserOnline());
      window.addEventListener('offline', () => {
        this.setStatus('warn', 'Your browser is offline - playback continues from the buffer, waiting for the network…');
        this.addEvent('warn', 'Browser went offline');
      });
    },

    async loadConfig() {
      try {
        const response = await fetch('/api/config');
        if (response.ok) this.config = await response.json();
      } catch (error) {
        // keep the defaults
      }
    },

    async loadLibrary() {
      this.loadingLibrary = true;
      try {
        const response = await fetch('/api/media');
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        this.library = await response.json();
        this.libraryLoaded = true;
        this.serverOnline = true;
        // Keep the selected item object up to date (sizes, stream state, ...).
        if (this.selected) {
          const fresh = [...this.library.videos, ...this.library.audios].find((i) => i.id === this.selected.id);
          if (fresh) this.selected = fresh;
        }
      } catch (error) {
        this.serverOnline = false;
      } finally {
        this.loadingLibrary = false;
      }
    },

    async refreshServerStats() {
      try {
        const response = await fetch('/api/stats');
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        this.serverStats = await response.json();
        this.serverOnline = true;
      } catch (error) {
        this.serverOnline = false;
      }
    },

    // ---------------------------------------------------------------- playback
    select(item) {
      this.selected = item;
      this.mode = 'hls';
      this.start();
    },

    // Starts (or restarts) playback of the selected item in the selected mode.
    async start() {
      this.destroyPlayer();
      const item = this.selected;
      if (!item) return;
      const token = ++playToken;
      this.playerStats = emptyPlayerStats();
      this.events = [];
      this.levels = [];
      this.selectedLevel = -1;
      this.startRequestedAt = performance.now();
      this.hasStartedPlaying = false;

      if (!item.playable) {
        this.setStatus('error', `This file cannot be read: ${item.probeError || 'unknown format'}`);
        return;
      }

      const video = this.$refs.video;
      if (this.mode === 'direct') {
        // Progressive download of the original file using HTTP Range requests.
        // Works only for formats the browser understands (e.g. MP4/H.264, MP3).
        this.playerStats.engine = 'Browser (progressive download)';
        this.setStatus('busy', 'Loading the original file…');
        video.src = item.urls.direct;
        video.play().catch(() => {});
        return;
      }

      // HLS: make sure the server has (or is producing) the adaptive ladder.
      this.setStatus('busy', 'Preparing the adaptive stream…');
      const ready = await this.waitUntilStreamReady(item, token);
      if (!ready || token !== playToken) return;
      this.attachHls(item, token);
    },

    // Asks the server to transcode the file and polls until the first
    // segments of every quality level exist.
    async waitUntilStreamReady(item, token) {
      this.prepareProgress = 0;
      try {
        let response = await fetch(item.urls.prepare, { method: 'POST' });
        let job = await response.json();
        while (token === playToken) {
          if (job.error && job.state === 'failed') {
            this.setStatus('error', `Transcoding failed: ${job.error}`);
            return false;
          }
          this.prepareProgress = job.progress || 0;
          if (job.ready) return true;
          const levels = (job.renditions || []).map((r) => r.label).join(', ');
          this.setStatus('busy', `Encoding H.264/AAC qualities (${levels})… ${Math.round(this.prepareProgress * 100)}%`);
          await sleep(1000);
          response = await fetch(item.urls.details);
          const details = await response.json();
          if (details.stream) job = details.stream;
        }
      } catch (error) {
        this.setStatus('error', `Cannot reach the server (${error.message}).`);
      }
      return false;
    },

    attachHls(item, token) {
      const video = this.$refs.video;
      const url = item.urls.hls;
      this.manifestLoaded = false;
      this.reconnectAttempts = 0;

      if (window.Hls && Hls.isSupported()) {
        this.playerStats.engine = `hls.js ${Hls.version}`;
        // Retry policy for downloads: several attempts with growing delays.
        // This is what keeps playback alive through short network drops.
        const retry = { maxNumRetry: 6, retryDelayMs: 1000, maxRetryDelayMs: 8000 };
        const policy = (timeoutMs) => ({
          default: { maxTimeToFirstByteMs: timeoutMs, maxLoadTimeMs: timeoutMs * 3, timeoutRetry: retry, errorRetry: retry },
        });
        hls = new Hls({
          startPosition: 0,          // start at the beginning even while the playlist still grows
          maxBufferLength: 30,       // seconds of media to keep downloaded ahead
          backBufferLength: 60,
          enableWorker: true,
          manifestLoadPolicy: policy(10000),
          playlistLoadPolicy: policy(10000),
          fragLoadPolicy: policy(20000),
        });

        hls.on(Hls.Events.MANIFEST_PARSED, () => {
          if (token !== playToken) return;
          this.manifestLoaded = true;
          this.levels = hls.levels.map((level, index) => ({ index, label: this.levelLabel(level), bitrate: level.bitrate }));
          this.addEvent('info', `Master playlist loaded: ${this.levels.length} quality levels`);
          video.play().catch(() => this.setStatus('idle', 'Ready - press play'));
        });
        hls.on(Hls.Events.LEVEL_SWITCHED, (_, data) => {
          if (token !== playToken) return;
          const level = hls.levels[data.level];
          this.playerStats.levelSwitches += 1;
          this.addEvent('info', `Quality is now ${this.levelLabel(level)}`);
        });
        hls.on(Hls.Events.FRAG_LOADED, (_, data) => {
          if (token !== playToken) return;
          this.playerStats.fragments += 1;
          const stats = data.frag.stats;
          const ms = Math.max(1, Math.round(stats.loading.end - stats.loading.start));
          this.playerStats.lastSegment = `${data.frag.relurl} · ${(stats.total / 1024).toFixed(0)} KB in ${ms} ms`;
          if (this.reconnectAttempts > 0) {
            this.reconnectAttempts = 0;
            this.addEvent('ok', 'Connection restored');
            if (this.status.kind !== 'ok') this.setStatus('ok', 'Connection restored - playing');
          }
        });
        hls.on(Hls.Events.ERROR, (_, data) => {
          if (token === playToken) this.onHlsError(data, item, token);
        });

        hls.loadSource(url);
        hls.attachMedia(video);
      } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
        // Safari plays HLS (including the quality switching) by itself.
        this.playerStats.engine = 'Native HLS (Safari)';
        video.src = url;
        video.play().catch(() => {});
      } else {
        this.setStatus('error', 'This browser cannot play HLS.');
      }
    },

    // hls.js reports errors here. "fatal" errors stop playback until we act.
    onHlsError(data, item, token) {
      if (!data.fatal) {
        this.playerStats.recoveredErrors += 1;  // hls.js already handled it (e.g. one retry)
        return;
      }
      if (data.type === Hls.ErrorTypes.NETWORK_ERROR) {
        this.scheduleReconnect(data.details, item, token);
      } else if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
        this.playerStats.recoveredErrors += 1;
        this.addEvent('warn', `Decoder problem (${data.details}) - recovering`);
        hls.recoverMediaError();
      } else {
        this.addEvent('error', `Player error: ${data.details} - restarting`);
        this.scheduleReconnect(data.details, item, token);
      }
    },

    // Waits 1, 2, 4, 8, 15, 15, ... seconds between attempts.
    scheduleReconnect(reason, item, token) {
      this.reconnectAttempts += 1;
      this.playerStats.reconnects += 1;
      const delay = Math.min(15000, 1000 * 2 ** (this.reconnectAttempts - 1));
      this.setStatus('warn', `Connection problem (${reason}). Retrying in ${Math.round(delay / 1000)} s (attempt ${this.reconnectAttempts})…`);
      this.addEvent('warn', `Network error: ${reason}`);
      clearTimeout(reconnectTimer);
      reconnectTimer = setTimeout(() => this.reconnect(item, token), delay);
    },

    reconnect(item, token) {
      if (token !== playToken || !hls) return;
      // hls.js now retries on its own schedule; the status stays until a
      // segment arrives ("Connection restored") or the next fatal error.
      this.setStatus('warn', `Reconnecting to the server (attempt ${this.reconnectAttempts})… playing from the buffer meanwhile`);
      if (this.manifestLoaded) {
        hls.startLoad(this.$refs.video.currentTime);  // continue where we stopped
      } else {
        hls.loadSource(item.urls.hls);                // the master playlist never arrived: load it again
      }
    },

    onBrowserOnline() {
      this.addEvent('ok', 'Browser is back online');
      if (hls && this.reconnectAttempts > 0 && this.selected) {
        clearTimeout(reconnectTimer);
        this.reconnect(this.selected, playToken);  // do not wait for the timer
      }
    },

    chooseLevel() {
      if (!hls) return;
      // -1 means automatic selection; any other number locks that level.
      hls.currentLevel = this.selectedLevel;
      this.addEvent('info', this.selectedLevel < 0 ? 'Automatic quality selection' : `Quality locked to ${this.levels[this.selectedLevel].label}`);
    },

    destroyPlayer() {
      clearTimeout(reconnectTimer);
      if (hls) {
        hls.destroy();
        hls = null;
      }
      const video = this.$refs.video;
      if (video) {
        video.pause();
        video.removeAttribute('src');
        video.load();
      }
    },

    // ---------------------------------------------------------------- <video> events
    onWaiting() {
      if (this.hasStartedPlaying) {
        this.playerStats.stalls += 1;
        this.setStatus('busy', 'Buffering…');
      }
    },
    onPlaying() {
      if (!this.hasStartedPlaying) {
        this.hasStartedPlaying = true;
        this.playerStats.startupMs = Math.round(performance.now() - this.startRequestedAt);
      }
      this.setStatus('ok', this.mode === 'hls' ? 'Playing (adaptive bitrate)' : 'Playing the original file');
    },
    onPause() {
      if (this.status.kind === 'ok') this.setStatus('idle', 'Paused');
    },
    onEnded() {
      this.setStatus('idle', 'Finished');
    },
    onVideoError() {
      const video = this.$refs.video;
      if (!video.error || hls) return;  // hls.js reports its own errors
      const hint = this.mode === 'direct' ? ' The browser may not support this file format - try HLS mode.' : '';
      this.setStatus('error', `Playback failed (code ${video.error.code}).${hint}`);
    },

    // ---------------------------------------------------------------- statistics
    updatePlayerStats() {
      const video = this.$refs.video;
      if (!video || !this.selected) return;
      const stats = this.playerStats;
      stats.position = video.currentTime || 0;
      stats.duration = Number.isFinite(video.duration) ? video.duration : 0;

      // Seconds of media already downloaded ahead of the playhead.
      stats.bufferAhead = 0;
      for (let i = 0; i < video.buffered.length; i += 1) {
        if (video.buffered.start(i) <= video.currentTime + 0.1 && video.buffered.end(i) >= video.currentTime) {
          stats.bufferAhead = video.buffered.end(i) - video.currentTime;
        }
      }

      if (hls) {
        const level = hls.levels[hls.currentLevel];
        stats.levelLabel = level ? this.levelLabel(level) : '';
        stats.levelBitrate = level ? level.bitrate : 0;
        stats.bandwidth = hls.bandwidthEstimate || 0;
      }
      if (video.getVideoPlaybackQuality) {
        const quality = video.getVideoPlaybackQuality();
        stats.droppedFrames = quality.droppedVideoFrames;
        stats.totalFrames = quality.totalVideoFrames;
      }
    },

    // ---------------------------------------------------------------- helpers
    setStatus(kind, text) {
      this.status = { kind, text };
    },

    addEvent(kind, text) {
      const time = new Date().toLocaleTimeString();
      this.events.unshift({ id: ++eventCounter, kind, text, time });
      this.events = this.events.slice(0, 8);
    },

    levelLabel(level) {
      if (!level) return '';
      const kbps = Math.round(level.bitrate / 1000);
      return level.height ? `${level.height}p · ${kbps} kbps` : `${kbps} kbps`;
    },

    // The coloured label next to each file in the library.
    badgeFor(item) {
      if (!item.playable) return { text: 'unreadable', css: 'bad' };
      const jobs = this.serverStats ? this.serverStats.transcoder.jobs : [];
      const job = jobs.find((j) => j.media === item.id) || item.stream;
      if (!job) return { text: 'not prepared', css: 'neutral' };
      if (job.state === 'completed') return { text: 'ready', css: 'good' };
      if (job.state === 'failed') return { text: 'failed', css: 'bad' };
      if (job.state === 'queued') return { text: 'queued', css: 'neutral' };
      return { text: `encoding ${Math.round((job.progress || 0) * 100)}%`, css: 'busy' };
    },

    async copy(text) {
      try {
        await navigator.clipboard.writeText(text);
      } catch (error) {
        // Clipboard API needs HTTPS or localhost; fall back to the old way.
        const area = document.createElement('textarea');
        area.value = text;
        document.body.appendChild(area);
        area.select();
        document.execCommand('copy');
        area.remove();
      }
      this.copiedText = text;
      setTimeout(() => { this.copiedText = ''; }, 1500);
    },

    formatBytes(bytes) {
      if (!bytes) return '0 B';
      const units = ['B', 'KB', 'MB', 'GB', 'TB'];
      let value = bytes;
      let unit = 0;
      while (value >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit += 1;
      }
      return `${value.toFixed(value < 10 && unit > 0 ? 1 : 0)} ${units[unit]}`;
    },

    formatDuration(seconds) {
      if (!seconds || !Number.isFinite(seconds)) return '0:00';
      const total = Math.floor(seconds);
      const h = Math.floor(total / 3600);
      const m = Math.floor((total % 3600) / 60);
      const s = String(total % 60).padStart(2, '0');
      return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${s}` : `${m}:${s}`;
    },

    formatBitrate(bitsPerSecond) {
      if (!bitsPerSecond) return '–';
      if (bitsPerSecond >= 1e6) return `${(bitsPerSecond / 1e6).toFixed(2)} Mbps`;
      return `${Math.round(bitsPerSecond / 1000)} kbps`;
    },
  }));
});
