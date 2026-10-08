// Cloud Gaming Engine - web client (Alpine.js).
//
// The browser is a "thin client": it never runs game code. It only
//   1. receives video frames over a WebSocket and paints them on a <canvas>,
//   2. sends mouse and keyboard input back to the server,
//   3. measures the connection (round-trip time, input-to-frame latency, ...).
//
// Binary message formats are documented in src/protocol/Protocol.hpp and the
// tile codec in src/codec/FrameCodec.hpp. All numbers are little-endian.

// ---------------------------------------------------------------------------
// Objects that must NOT live inside Alpine's reactive state (Alpine wraps
// state in proxies, which would make per-frame work slow).
// ---------------------------------------------------------------------------
let socket = null;          // the WebSocket
let context2d = null;       // canvas drawing context
let image = null;           // ImageData = the decoded picture (RGBA)
let needsPaint = false;     // a new frame was decoded since the last paint
let reconnectTimer = null;
let pingTimer = null;
let leaving = false;        // true when the user leaves on purpose (no reconnect)
let inputSequence = 0;
let pendingMove = null;     // latest pointer position, sent once per animation frame
const inputSentAt = new Map();  // input sequence -> performance.now() when sent

// Raw counters for the current one-second measuring window.
const counters = { frames: 0, draws: 0, bytes: 0, decodeMs: 0, intervals: [], lastArrival: 0 };

const MSG_JOIN = 0x01, MSG_INPUT = 0x02, MSG_PING = 0x03, MSG_KEYFRAME = 0x04;
const MSG_FRAME = 0x10, MSG_PONG = 0x11;
const INPUT = { pointerDown: 1, pointerUp: 2, pointerMove: 3, keyDown: 4, keyUp: 5 };
const TILE = 16;

const encoder = new TextEncoder();

// Builds a binary message from a list of [type, value] parts.
function buildMessage(parts) {
  const bytes = [];
  const scratch = new DataView(new ArrayBuffer(8));
  for (const [type, value] of parts) {
    if (type === 'u8') bytes.push(value & 0xff);
    else if (type === 'u32') { scratch.setUint32(0, value >>> 0, true); bytes.push(...new Uint8Array(scratch.buffer, 0, 4)); }
    else if (type === 'i16') { scratch.setInt16(0, value, true); bytes.push(...new Uint8Array(scratch.buffer, 0, 2)); }
    else if (type === 'f32') { scratch.setFloat32(0, value, true); bytes.push(...new Uint8Array(scratch.buffer, 0, 4)); }
    else if (type === 'f64') { scratch.setFloat64(0, value, true); bytes.push(...new Uint8Array(scratch.buffer, 0, 8)); }
    else if (type === 'str') {  // u8 length + UTF-8 bytes (max 255)
      const text = encoder.encode(value).slice(0, 255);
      bytes.push(text.length, ...text);
    }
  }
  return new Uint8Array(bytes);
}

// Decodes CGV1 tile data (see FrameCodec.hpp) straight into the ImageData.
function applyTiles(data, offset, pixels, width, height) {
  const tilesX = Math.ceil(width / TILE);
  let pos = offset;
  const count = data[pos] | (data[pos + 1] << 8);
  pos += 2;
  for (let t = 0; t < count; t++) {
    const index = data[pos] | (data[pos + 1] << 8);
    const mode = data[pos + 2];
    pos += 3;
    const x0 = (index % tilesX) * TILE;
    const y0 = Math.floor(index / tilesX) * TILE;
    const w = Math.min(TILE, width - x0);
    const h = Math.min(TILE, height - y0);
    if (x0 >= width || y0 >= height) throw new Error('tile outside the picture');

    if (mode === 0) {          // SOLID: one colour for the whole tile
      const r = data[pos], g = data[pos + 1], b = data[pos + 2];
      pos += 3;
      for (let y = 0; y < h; y++) {
        let p = ((y0 + y) * width + x0) * 4;
        for (let x = 0; x < w; x++, p += 4) { pixels[p] = r; pixels[p + 1] = g; pixels[p + 2] = b; pixels[p + 3] = 255; }
      }
    } else if (mode === 1) {   // RLE: runs of equal pixels, row by row
      const runs = data[pos] | (data[pos + 1] << 8);
      pos += 2;
      let i = 0;
      for (let k = 0; k < runs; k++) {
        const length = data[pos], r = data[pos + 1], g = data[pos + 2], b = data[pos + 3];
        pos += 4;
        for (let n = 0; n < length; n++, i++) {
          const p = ((y0 + Math.floor(i / w)) * width + x0 + (i % w)) * 4;
          pixels[p] = r; pixels[p + 1] = g; pixels[p + 2] = b; pixels[p + 3] = 255;
        }
      }
      if (i !== w * h) throw new Error('bad RLE tile');
    } else if (mode === 2) {   // RAW: every pixel as R, G, B
      for (let y = 0; y < h; y++) {
        let p = ((y0 + y) * width + x0) * 4;
        for (let x = 0; x < w; x++, p += 4, pos += 3) {
          pixels[p] = data[pos]; pixels[p + 1] = data[pos + 1]; pixels[p + 2] = data[pos + 2]; pixels[p + 3] = 255;
        }
      }
    } else {
      throw new Error('unknown tile mode ' + mode);
    }
  }
  if (pos !== data.length) throw new Error('unexpected bytes after the tiles');
}

document.addEventListener('alpine:init', () => {
  Alpine.data('cloudGaming', () => ({
    // ------------------------------------------------------------ shared state
    view: 'lobby',
    serverOnline: true,

    // lobby
    games: [],
    sessions: [],
    gamesFolder: 'games/',
    lobbyLoaded: false,
    lobbyError: '',
    creating: '',

    // play page
    sessionId: '',
    sessionInfo: null,
    sessionGone: false,
    game: null,
    playerName: '',
    joined: false,
    connecting: false,
    connectionState: 'idle',   // idle | connecting | live | reconnecting | ended
    role: '',
    myPlayer: -1,
    viewerId: 0,
    gameStatus: '',
    players: [],
    gameMetrics: [],
    engine: null,
    viewers: [],
    overlay: '',
    errorMessage: '',
    copied: false,
    reconnectAttempts: 0,
    net: {
      rtt: 0, rttAvg: 0, inputLatencyLast: 0, inputLatencyAvg: 0, inputLatencyMax: 0,
      fps: 0, drawFps: 0, kbps: 0, avgFrameBytes: 0, keyframes: 0, deltas: 0,
      decodeMs: 0, jitter: 0, inputsSent: 0, reconnects: 0,
    },

    get shareUrl() {
      return `${location.origin}/play/${this.sessionId}`;
    },
    get myViewer() {
      return this.viewers.find((v) => v.id === this.viewerId) || null;
    },

    // ------------------------------------------------------------ start-up
    init() {
      const match = location.pathname.match(/^\/play\/([^/]+)/);
      if (match) {
        this.view = 'play';
        this.sessionId = decodeURIComponent(match[1]);
        this.playerName = localStorage.getItem('cge.name') || '';
        this.loadSessionInfo().then(() => {
          // Reloaded this tab while playing? Rejoin automatically with the saved seat token.
          if (!this.sessionGone && sessionStorage.getItem(this.tokenKey())) this.join(false);
        });
        this.setupCanvas();
        setInterval(() => this.updateNetworkStats(), 1000);
        setInterval(() => { if (!this.joined) this.loadSessionInfo(); }, 3000);
      } else {
        this.refreshLobby();
        setInterval(() => this.refreshLobby(), 3000);
      }
    },

    // ------------------------------------------------------------ lobby
    async refreshLobby() {
      try {
        const [gamesResponse, sessionsResponse] = await Promise.all([fetch('/api/games'), fetch('/api/sessions')]);
        const gamesBody = await gamesResponse.json();
        const sessionsBody = await sessionsResponse.json();
        // Playable games first, then the ones with problems.
        this.games = gamesBody.games.sort((a, b) => (b.ready - a.ready) || a.name.localeCompare(b.name));
        this.gamesFolder = gamesBody.gamesFolder;
        this.sessions = sessionsBody.sessions;
        this.lobbyLoaded = true;
        this.serverOnline = true;
      } catch (error) {
        this.serverOnline = false;
      }
    },

    async createSession(game) {
      this.creating = game.id;
      this.lobbyError = '';
      try {
        const response = await fetch(`/api/games/${encodeURIComponent(game.id)}/sessions`, { method: 'POST' });
        const body = await response.json();
        if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
        location.assign(body.url);  // open the game page of the new session
      } catch (error) {
        this.lobbyError = `Could not start ${game.name}: ${error.message}`;
        this.creating = '';
      }
    },

    // ------------------------------------------------------------ play page: before joining
    async loadSessionInfo() {
      try {
        const response = await fetch(`/api/sessions/${encodeURIComponent(this.sessionId)}`);
        if (response.status === 404) {
          this.sessionGone = true;
          return;
        }
        this.sessionInfo = await response.json();
        if (!this.game) this.game = this.sessionInfo.game;
        if (!this.joined) this.players = this.sessionInfo.players;
        this.serverOnline = true;
      } catch (error) {
        this.serverOnline = false;
      }
    },

    seatsText() {
      if (!this.sessionInfo) return '';
      const free = this.sessionInfo.maxPlayers - this.sessionInfo.playersTaken;
      if (free <= 0) return 'All player seats are taken - you can join as a spectator.';
      return `${free} of ${this.sessionInfo.maxPlayers} player seat${free === 1 ? '' : 's'} free.`;
    },

    // The seat token is kept in sessionStorage: it belongs to THIS browser tab
    // (it survives a reload, so a player gets their seat back), while a second
    // tab of the same browser is a new person and gets its own seat.
    tokenKey() {
      return `cge.token.${this.sessionId}`;
    },

    setupCanvas() {
      this.$nextTick(() => {
        const canvas = this.$refs.screen;
        context2d = canvas.getContext('2d');
        canvas.addEventListener('pointerdown', (e) => this.onPointer(e, INPUT.pointerDown));
        canvas.addEventListener('pointerup', (e) => this.onPointer(e, INPUT.pointerUp));
        canvas.addEventListener('pointermove', (e) => this.onPointer(e, INPUT.pointerMove));
        canvas.addEventListener('keydown', (e) => this.onKey(e, INPUT.keyDown));
        canvas.addEventListener('keyup', (e) => this.onKey(e, INPUT.keyUp));
        requestAnimationFrame(() => this.paintLoop());
      });
    },

    // ------------------------------------------------------------ connection
    join(spectate) {
      leaving = false;
      this.errorMessage = '';
      const name = (this.playerName || '').trim() || `Player${Math.floor(Math.random() * 900 + 100)}`;
      this.playerName = name;
      localStorage.setItem('cge.name', name);
      this.connect(spectate);
    },

    connect(spectate) {
      clearTimeout(reconnectTimer);
      this.connecting = true;
      this.connectionState = this.reconnectAttempts > 0 ? 'reconnecting' : 'connecting';
      this.overlay = this.reconnectAttempts > 0 ? 'Reconnecting…' : 'Connecting…';
      const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
      socket = new WebSocket(`${protocol}://${location.host}/ws/${encodeURIComponent(this.sessionId)}`);
      socket.binaryType = 'arraybuffer';

      socket.onopen = () => {
        // JOIN: name, the saved token (to get our old seat back) and the spectate flag.
        const token = sessionStorage.getItem(this.tokenKey()) || '';
        socket.send(buildMessage([['u8', MSG_JOIN], ['str', this.playerName], ['str', token], ['u8', spectate ? 1 : 0]]));
        clearInterval(pingTimer);
        pingTimer = setInterval(() => this.sendPing(), 1000);
        this.sendPing();
      };
      socket.onmessage = (event) => {
        if (typeof event.data === 'string') this.onTextMessage(JSON.parse(event.data));
        else this.onBinaryMessage(event.data);
      };
      socket.onclose = () => this.onDisconnected(spectate);
      socket.onerror = () => {};  // onclose follows and handles everything
    },

    onDisconnected(spectate) {
      clearInterval(pingTimer);
      socket = null;
      if (leaving || this.connectionState === 'ended') return;
      // Lost the connection: try again with growing pauses (1, 2, 4, 8 s, then every 10 s).
      this.reconnectAttempts += 1;
      this.net.reconnects += 1;
      this.connectionState = 'reconnecting';
      const delay = Math.min(10000, 1000 * 2 ** (this.reconnectAttempts - 1));
      this.overlay = `Connection lost - reconnecting in ${Math.round(delay / 1000)} s…`;
      reconnectTimer = setTimeout(async () => {
        await this.loadSessionInfo();
        if (this.sessionGone) {
          this.connectionState = 'ended';
          this.overlay = 'This session has ended.';
          return;
        }
        this.connect(spectate);
      }, delay);
    },

    leave() {
      leaving = true;
      if (socket) socket.close();
    },

    sendPing() {
      if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(buildMessage([['u8', MSG_PING], ['f64', performance.now()], ['f32', this.net.rtt]]));
      }
    },

    // ------------------------------------------------------------ messages from the server
    onTextMessage(message) {
      if (message.type === 'welcome') {
        this.joined = true;
        this.connecting = false;
        this.connectionState = 'live';
        this.overlay = '';
        this.reconnectAttempts = 0;
        this.role = message.role;
        this.myPlayer = message.player;
        this.viewerId = message.viewerId;
        this.game = message.game;
        if (message.token) sessionStorage.setItem(this.tokenKey(), message.token);
        inputSequence = 0;  // the server counts this connection's inputs from 1 again
        inputSentAt.clear();
        const canvas = this.$refs.screen;
        canvas.width = message.game.width;
        canvas.height = message.game.height;
        image = context2d.createImageData(message.game.width, message.game.height);
        if (this.role === 'player') canvas.focus();
      } else if (message.type === 'state') {
        this.gameStatus = message.status;
        this.players = message.players;
        this.gameMetrics = message.gameMetrics;
        this.engine = message.engine;
        this.viewers = message.viewers;
      } else if (message.type === 'error') {
        this.errorMessage = message.message;
        if (/ended|full/.test(message.message)) {
          this.connectionState = 'ended';
          this.overlay = message.message;
        }
      }
    },

    onBinaryMessage(buffer) {
      const data = new Uint8Array(buffer);
      const view = new DataView(buffer);
      if (data[0] === MSG_PONG) {
        const rtt = performance.now() - view.getFloat64(1, true);
        this.net.rtt = rtt;
        this.net.rttAvg = this.net.rttAvg ? this.net.rttAvg * 0.8 + rtt * 0.2 : rtt;
        return;
      }
      if (data[0] !== MSG_FRAME || !image) return;

      // ---- header
      const keyframe = (data[1] & 1) === 1;
      const width = view.getUint16(14, true);
      const height = view.getUint16(16, true);
      const ackCount = data[22];
      let offset = 23;
      for (let i = 0; i < ackCount; i++, offset += 5) {
        const player = data[offset];
        const sequence = view.getUint32(offset + 1, true);
        if (player === this.myPlayer) this.onInputAcknowledged(sequence);
      }
      if (width !== image.width || height !== image.height) return;

      // ---- tiles
      const started = performance.now();
      try {
        applyTiles(data, offset, image.data, width, height);
      } catch (error) {
        // Broken frame: ask for a complete picture instead of showing garbage.
        console.warn('Frame could not be decoded:', error.message);
        socket.send(new Uint8Array([MSG_KEYFRAME]));
        return;
      }
      counters.decodeMs += performance.now() - started;
      needsPaint = true;

      // ---- statistics
      const now = performance.now();
      if (counters.lastArrival) counters.intervals.push(now - counters.lastArrival);
      counters.lastArrival = now;
      counters.frames += 1;
      counters.bytes += data.length;
      if (keyframe) this.net.keyframes += 1;
      else this.net.deltas += 1;
    },

    // Each frame says up to which input sequence number our inputs are
    // already visible in the picture: that gives the input-to-frame latency.
    onInputAcknowledged(sequence) {
      const now = performance.now();
      for (const [seq, sentAt] of inputSentAt) {
        if (seq > sequence) break;
        const latency = now - sentAt;
        inputSentAt.delete(seq);
        this.net.inputLatencyLast = latency;
        this.net.inputLatencyAvg = this.net.inputLatencyAvg ? this.net.inputLatencyAvg * 0.8 + latency * 0.2 : latency;
        this.net.inputLatencyMax = Math.max(this.net.inputLatencyMax, latency);
      }
    },

    // Paint at most once per screen refresh, however many frames arrived.
    paintLoop() {
      if (needsPaint && image) {
        context2d.putImageData(image, 0, 0);
        needsPaint = false;
        counters.draws += 1;
      }
      if (pendingMove) {  // pointer moves are sent at most once per refresh
        this.sendInput(INPUT.pointerMove, pendingMove.x, pendingMove.y, 0, '');
        pendingMove = null;
      }
      requestAnimationFrame(() => this.paintLoop());
    },

    updateNetworkStats() {
      this.net.fps = counters.frames;
      this.net.drawFps = counters.draws;
      this.net.kbps = (counters.bytes * 8) / 1000;
      this.net.avgFrameBytes = counters.frames ? counters.bytes / counters.frames : 0;
      this.net.decodeMs = counters.frames ? counters.decodeMs / counters.frames : 0;
      // Jitter = how much the time between frames varies (standard deviation).
      const intervals = counters.intervals;
      if (intervals.length > 1) {
        const mean = intervals.reduce((a, b) => a + b, 0) / intervals.length;
        this.net.jitter = Math.sqrt(intervals.reduce((a, b) => a + (b - mean) ** 2, 0) / intervals.length);
      }
      counters.frames = 0;
      counters.draws = 0;
      counters.bytes = 0;
      counters.decodeMs = 0;
      counters.intervals = [];
    },

    // ------------------------------------------------------------ input forwarding
    sendInput(kind, x, y, button, key) {
      if (!socket || socket.readyState !== WebSocket.OPEN || this.role !== 'player') return;
      inputSequence += 1;
      inputSentAt.set(inputSequence, performance.now());
      if (inputSentAt.size > 500) inputSentAt.delete(inputSentAt.keys().next().value);
      socket.send(buildMessage([
        ['u8', MSG_INPUT], ['u8', kind], ['u32', inputSequence], ['i16', x], ['i16', y], ['u8', button], ['str', key],
      ]));
      this.net.inputsSent += 1;
    },

    // Converts screen coordinates to canvas pixels (the canvas may be scaled by CSS).
    onPointer(event, kind) {
      const canvas = this.$refs.screen;
      const rect = canvas.getBoundingClientRect();
      const x = Math.round(((event.clientX - rect.left) * canvas.width) / rect.width);
      const y = Math.round(((event.clientY - rect.top) * canvas.height) / rect.height);
      if (kind === INPUT.pointerMove) {
        pendingMove = { x, y };
        return;
      }
      if (kind === INPUT.pointerDown) canvas.focus();
      this.sendInput(kind, x, y, event.button || 0, '');
    },

    onKey(event, kind) {
      if (event.repeat && kind === INPUT.keyDown) return;  // ignore auto-repeat
      if ([' ', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(event.key)) event.preventDefault();
      this.sendInput(kind, 0, 0, 0, event.key);
    },

    // ------------------------------------------------------------ helpers
    async copyLink() {
      try {
        await navigator.clipboard.writeText(this.shareUrl);
      } catch (error) {
        // The clipboard API needs HTTPS or localhost; fall back to the old way.
        const area = document.createElement('textarea');
        area.value = this.shareUrl;
        document.body.appendChild(area);
        area.select();
        document.execCommand('copy');
        area.remove();
      }
      this.copied = true;
      setTimeout(() => { this.copied = false; }, 1500);
    },

    roleText() {
      return this.role === 'player' ? `player ${this.myPlayer + 1}` : 'a spectator';
    },

    connectionText() {
      return {
        idle: 'Not connected',
        connecting: 'Connecting…',
        live: 'Connected',
        reconnecting: 'Reconnecting…',
        ended: 'Session ended',
      }[this.connectionState];
    },

    playersText(game) {
      return game.minPlayers === game.maxPlayers ? `${game.maxPlayers} players` : `${game.minPlayers}-${game.maxPlayers} players`;
    },

    statusLabel(status) {
      return { ready: 'ready', building: 'building…', build_failed: 'compile error', invalid: 'rejected' }[status] || status;
    },

    badgeClass(status) {
      return { ready: 'good', building: 'busy', build_failed: 'bad', invalid: 'bad' }[status] || '';
    },

    ms(value) {
      return value ? `${value.toFixed(1)} ms` : '–';
    },

    bytes(value) {
      if (!value) return '0 B';
      if (value < 1024) return `${Math.round(value)} B`;
      if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`;
      return `${(value / 1024 / 1024).toFixed(2)} MB`;
    },

    formatDuration(seconds) {
      const total = Math.floor(seconds || 0);
      const h = Math.floor(total / 3600);
      const m = Math.floor((total % 3600) / 60);
      const s = String(total % 60).padStart(2, '0');
      return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${s}` : `${m}:${s}`;
    },
  }));
});
