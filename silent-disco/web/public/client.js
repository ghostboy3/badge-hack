// Silent Disco phone client. See docs/web-sync-protocol.md for the wire
// protocol, offset-estimation algorithm, and playlist-position math, and
// docs/follow-badge-protocol.md for the optional "Follow my badge" Web
// Bluetooth feature (Android Chrome only -- feature-detected below, never
// required; manual channel taps always work regardless).
//
// Three channels (CLAUDE.md rule 6), each a real playlist ("preloads all 3
// tracks, plays them in lockstep at gain 0, and crossfades to the selected
// one -- no resync"): one <audio> element per channel, all three playing
// continuously from the moment you join (muted except the selected one),
// each independently advancing through its own playlist and recomputing
// its position from the server clock on every track boundary. Switching
// channels is *only* a gain crossfade -- the non-selected channels were
// already correctly positioned in the background.
//
// Playback isn't quantized to a beat grid at all (that only matters for
// the BLE/LED side, badge/main/*, which is unrelated -- see
// docs/web-sync-protocol.md's "known gap"). Cross-phone sync is limited to
// "which track + what position," reusing the same clock-offset estimate
// this file already computed for that.
(() => {
  const PING_INTERVAL_MS = 1000;
  const SAMPLE_WINDOW_MS = 10000; // trailing window for the lowest-RTT filter
  const CROSSFADE_S = 0.12;       // channel-switch gain ramp
  const CHANNEL_GAIN = 0.5;

  // docs/follow-badge-protocol.md -- keep in sync with
  // badge/main/ble_channel_service.c's s_svc_uuid/s_chr_uuid.
  const FOLLOW_SERVICE_UUID = '9ac33a43-5fe6-40a9-8a5f-921a1a8933e8';
  const FOLLOW_CHANNEL_CHAR_UUID = '4e6c2458-d8ee-4401-8b35-41819926f033';

  // Matches badge/main/main.c's CHANNELS table for color/name; the badge
  // renders color+LEDs, we render audio -- index order also matches
  // server.js's CHANNEL_DIRS (['pink', 'orange', 'purple']).
  const CHANNELS = [
    { name: 'Pink' },
    { name: 'Orange' },
    { name: 'Purple' },
  ];

  const statusEl = document.getElementById('status');
  const joinBtn = document.getElementById('join');
  const pulseEl = document.getElementById('pulse');
  const channelsEl = document.getElementById('channels');
  const channelBtns = [...document.querySelectorAll('.channel-btn')];
  const statsEl = document.getElementById('stats');
  const statChannel = document.getElementById('stat-channel');
  const statTrack = document.getElementById('stat-track');
  const statOffset = document.getElementById('stat-offset');
  const statRtt = document.getElementById('stat-rtt');
  const followBtn = document.getElementById('follow-badge');
  const followStatusEl = document.getElementById('follow-status');

  /** @type {{playlist: {url: string, title: string, durationSec: number}[]}[]} */
  let channels = [];
  let playlistEpochMs = null;

  /** @type {{offset: number, rtt: number, time: number}[]} */
  let samples = [];
  let offsetEstimate = 0;

  let audioCtx = null;
  let analyser = null;
  let pulseData = null;
  /** @type {HTMLAudioElement[]} */
  let audioEls = [];
  /** @type {GainNode[]} one per channel, all routed through `analyser` to destination */
  let channelGains = [];
  /** @type {({url,title,durationSec}|null)[]} currently-playing track per channel, for display */
  let currentTrack = [];
  let selectedChannel = 0;
  let playing = false;

  function setStatus(text) {
    statusEl.textContent = text;
  }

  function serverTimeNow() {
    return Date.now() + offsetEstimate;
  }

  function recordSample(offset, rtt) {
    const now = Date.now();
    samples.push({ offset, rtt, time: now });
    samples = samples.filter((s) => now - s.time <= SAMPLE_WINDOW_MS);

    let best = samples[0];
    for (const s of samples) {
      if (s.rtt < best.rtt) best = s;
    }
    offsetEstimate = best.offset;

    statOffset.textContent = `${offsetEstimate.toFixed(1)} ms`;
    statRtt.textContent = `${best.rtt.toFixed(1)} ms`;
  }

  // Where in its playlist channel `channels[i]` should be right now, given
  // `serverNow` -- the whole playlist loops forever from playlistEpochMs.
  // Returns null if the channel has no tracks yet (empty folder).
  function computePosition(channel, serverNow) {
    const total = channel.playlist.reduce((s, t) => s + t.durationSec, 0);
    if (total <= 0) return null;

    let elapsed = ((serverNow - playlistEpochMs) / 1000) % total;
    if (elapsed < 0) elapsed += total;

    for (let i = 0; i < channel.playlist.length; i++) {
      const dur = channel.playlist[i].durationSec;
      if (elapsed < dur) return { trackIndex: i, offsetSec: elapsed };
      elapsed -= dur;
    }
    return { trackIndex: 0, offsetSec: 0 }; // floating-point rounding fallback
  }

  function updateNowPlayingDisplay() {
    const track = currentTrack[selectedChannel];
    statTrack.textContent = track ? track.title : 'No tracks yet';
  }

  // (Re)starts channel `i` at wherever computePosition says it should be
  // right now. Used both on join and as the `ended` handler -- recomputing
  // fresh from the server clock on every track boundary self-corrects any
  // drift instead of needing continuous mid-track re-sync.
  function startChannelPlayback(i) {
    const channel = channels[i];
    const pos = computePosition(channel, serverTimeNow());
    currentTrack[i] = pos ? channel.playlist[pos.trackIndex] : null;
    if (i === selectedChannel) updateNowPlayingDisplay();

    const audioEl = audioEls[i];
    if (!pos) {
      audioEl.pause();
      audioEl.removeAttribute('src');
      return;
    }

    const track = channel.playlist[pos.trackIndex];
    audioEl.addEventListener('loadedmetadata', () => {
      audioEl.currentTime = pos.offsetSec;
      audioEl.play().catch((err) => console.error(`channel ${i} play() failed:`, err));
    }, { once: true });
    audioEl.src = track.url;
    audioEl.load();
  }

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${proto}://${location.host}`);

    ws.addEventListener('open', () => setStatus('Connected, syncing clock…'));

    ws.addEventListener('message', (event) => {
      const msg = JSON.parse(event.data);

      if (msg.type === 'hello') {
        playlistEpochMs = msg.playlistEpochMs;
        channels = msg.channels;
        currentTrack = channels.map(() => null);
        statsEl.hidden = false;

        // First ping immediately, then steady interval.
        sendPing(ws);
        setInterval(() => sendPing(ws), PING_INTERVAL_MS);
      } else if (msg.type === 'pong') {
        const t2 = Date.now();
        const rtt = t2 - msg.t0;
        const offset = ((msg.t1 - msg.t0) + (msg.t1 - t2)) / 2;
        recordSample(offset, rtt);

        if (joinBtn.disabled) {
          joinBtn.disabled = false;
          setStatus('Synced. Tap to join.');
        }
      }
    });

    ws.addEventListener('close', () => {
      setStatus('Disconnected. Reload to retry.');
      joinBtn.disabled = true;
    });
  }

  function sendPing(ws) {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'ping', t0: Date.now() }));
    }
  }

  function selectChannel(channel) {
    if (channel === selectedChannel) return;

    const now = audioCtx.currentTime;
    channelGains[selectedChannel].gain.cancelScheduledValues(now);
    channelGains[selectedChannel].gain.linearRampToValueAtTime(0, now + CROSSFADE_S);
    channelGains[channel].gain.cancelScheduledValues(now);
    channelGains[channel].gain.linearRampToValueAtTime(CHANNEL_GAIN, now + CROSSFADE_S);

    selectedChannel = channel;
    document.body.dataset.channel = String(channel);
    statChannel.textContent = CHANNELS[channel].name;
    updateNowPlayingDisplay();
    for (const btn of channelBtns) {
      btn.classList.toggle('selected', Number(btn.dataset.channel) === channel);
    }
  }

  // Drives the pulse circle from the actually-audible channel's real audio
  // (via `analyser`, fed by all channel gain nodes) instead of a beat grid.
  function pulseTick() {
    if (!playing) return;

    analyser.getByteTimeDomainData(pulseData);
    let sumSquares = 0;
    for (let i = 0; i < pulseData.length; i++) {
      const v = (pulseData[i] - 128) / 128;
      sumSquares += v * v;
    }
    const rms = Math.sqrt(sumSquares / pulseData.length); // ~0 (silence) .. ~1 (loud)
    const loudness = Math.min(1, rms * 4); // headroom so quieter tracks still read as visible motion

    pulseEl.style.opacity = String(0.25 + 0.75 * loudness);
    pulseEl.style.transform = `scale(${0.85 + 0.2 * loudness})`;
    pulseEl.classList.toggle('on', loudness > 0.5);

    requestAnimationFrame(pulseTick);
  }

  channelBtns.forEach((btn) => {
    btn.addEventListener('click', () => selectChannel(Number(btn.dataset.channel)));
  });

  function setFollowStatus(text) {
    followStatusEl.textContent = text;
    followStatusEl.hidden = !text;
  }

  // Applies a channel index received from the badge. If we haven't joined
  // yet (no AudioContext/gain nodes), selectChannel() would have nothing
  // to crossfade -- just remember the choice for when Join is tapped.
  function applyChannelFromBadge(channel) {
    if (!Number.isInteger(channel) || channel < 0 || channel >= CHANNELS.length) {
      return;
    }
    if (playing) {
      selectChannel(channel);
    } else {
      selectedChannel = channel;
    }
  }

  async function followBadge() {
    try {
      setFollowStatus('Choose your badge…');
      const device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [FOLLOW_SERVICE_UUID] }],
      });
      device.addEventListener('gattserverdisconnected', () => {
        setFollowStatus('Badge disconnected.');
      });

      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(FOLLOW_SERVICE_UUID);
      const characteristic = await service.getCharacteristic(FOLLOW_CHANNEL_CHAR_UUID);

      const initial = await characteristic.readValue();
      applyChannelFromBadge(initial.getUint8(0));

      characteristic.addEventListener('characteristicvaluechanged', (event) => {
        applyChannelFromBadge(event.target.value.getUint8(0));
      });
      await characteristic.startNotifications();

      setFollowStatus(`Following ${device.name || 'your badge'}.`);
    } catch (err) {
      console.error('follow my badge failed:', err);
      setFollowStatus('Could not connect to a badge.');
    }
  }

  // Web Bluetooth is Android Chrome only (no iOS Safari, no desktop
  // Firefox/Safari) -- feature-detected, and the button stays hidden
  // everywhere else. Manual channel taps above always work regardless.
  if ('bluetooth' in navigator) {
    followBtn.hidden = false;
    followBtn.addEventListener('click', followBadge);
  }

  joinBtn.addEventListener('click', async () => {
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      analyser = audioCtx.createAnalyser();
      analyser.fftSize = 256;
      analyser.connect(audioCtx.destination);
      pulseData = new Uint8Array(analyser.frequencyBinCount);

      for (let i = 0; i < channels.length; i++) {
        const audioEl = new Audio();
        audioEl.preload = 'auto';
        audioEl.addEventListener('ended', () => startChannelPlayback(i));

        const gain = audioCtx.createGain();
        gain.gain.value = 0;
        audioCtx.createMediaElementSource(audioEl).connect(gain).connect(analyser);

        audioEls[i] = audioEl;
        channelGains[i] = gain;
      }
    }
    await audioCtx.resume();

    channelGains[selectedChannel].gain.value = CHANNEL_GAIN;
    document.body.dataset.channel = String(selectedChannel);
    channelBtns[selectedChannel].classList.add('selected');
    statChannel.textContent = CHANNELS[selectedChannel].name;
    channelsEl.hidden = false;

    for (let i = 0; i < channels.length; i++) {
      startChannelPlayback(i);
    }

    playing = true;
    joinBtn.hidden = true;
    setStatus('Playing.');
    requestAnimationFrame(pulseTick);
  });

  connect();
})();
