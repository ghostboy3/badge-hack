// Silent Disco phone client -- steps 2 & 4a. See docs/web-sync-protocol.md
// for the wire protocol and offset-estimation algorithm.
//
// Step 4a (three channels): CLAUDE.md says the phone "preloads all 3
// tracks, plays them in lockstep at gain 0, and crossfades to the selected
// one (no resync)". There are no real tracks yet (that's the lookahead
// pattern schedule, the next build-order step) -- each channel is a
// distinct synthesized tone standing in for its track, but the lockstep +
// per-channel gain node + crossfade structure is the real thing: every
// channel is scheduled on every beat, only the selected one is audible.
(() => {
  const PING_INTERVAL_MS = 1000;
  const SAMPLE_WINDOW_MS = 10000; // trailing window for the lowest-RTT filter
  const LOOKAHEAD_MS = 200;       // schedule beats this far ahead of "now"
  const PULSE_WIDTH_MS = 90;      // visual pulse duration, mirrors the badge's LED flash
  const CROSSFADE_S = 0.12;       // channel-switch gain ramp
  const CHANNEL_GAIN = 0.5;

  // Matches badge/main/main.c's CHANNELS table (name + color; the badge
  // renders color, we render a distinct placeholder tone per channel).
  const CHANNELS = [
    { name: 'Pink', freq: 880 },
    { name: 'Orange', freq: 587 },
    { name: 'Purple', freq: 1175 },
  ];

  const statusEl = document.getElementById('status');
  const joinBtn = document.getElementById('join');
  const pulseEl = document.getElementById('pulse');
  const channelsEl = document.getElementById('channels');
  const channelBtns = [...document.querySelectorAll('.channel-btn')];
  const statsEl = document.getElementById('stats');
  const statChannel = document.getElementById('stat-channel');
  const statBpm = document.getElementById('stat-bpm');
  const statOffset = document.getElementById('stat-offset');
  const statRtt = document.getElementById('stat-rtt');

  let bpm = null;
  let beatEpochMs = null;

  /** @type {{offset: number, rtt: number, time: number}[]} */
  let samples = [];
  let offsetEstimate = 0;

  let audioCtx = null;
  /** @type {GainNode[]} one per channel, all connected to destination */
  let channelGains = [];
  let selectedChannel = 0;
  let playing = false;
  let nextBeatIndex = null; // next not-yet-scheduled beat number

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

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${proto}://${location.host}`);

    ws.addEventListener('open', () => setStatus('Connected, syncing clock…'));

    ws.addEventListener('message', (event) => {
      const msg = JSON.parse(event.data);

      if (msg.type === 'hello') {
        bpm = msg.bpm;
        beatEpochMs = msg.beatEpochMs;
        statBpm.textContent = String(bpm);
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

  function playClick(channel, audioTime) {
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = 'sine';
    osc.frequency.value = CHANNELS[channel].freq;
    gain.gain.setValueAtTime(0.0001, audioTime);
    gain.gain.exponentialRampToValueAtTime(0.5, audioTime + 0.005);
    gain.gain.exponentialRampToValueAtTime(0.0001, audioTime + 0.08);
    osc.connect(gain).connect(channelGains[channel]);
    osc.start(audioTime);
    osc.stop(audioTime + 0.1);
  }

  function pulseVisual(onMs) {
    pulseEl.classList.add('on');
    setTimeout(() => pulseEl.classList.remove('on'), onMs);
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
    for (const btn of channelBtns) {
      btn.classList.toggle('selected', Number(btn.dataset.channel) === channel);
    }
  }

  function schedulerTick() {
    if (!playing) return;

    const beatPeriodMs = 60000 / bpm;
    const now = serverTimeNow();

    if (nextBeatIndex === null || beatEpochMs + nextBeatIndex * beatPeriodMs < now - beatPeriodMs) {
      // First run, or we fell far behind (e.g. the tab was backgrounded and
      // rAF paused) -- resync to the current beat instead of bursting
      // through every beat that was missed.
      nextBeatIndex = Math.ceil((now - beatEpochMs) / beatPeriodMs);
    }

    while (true) {
      const beatServerTime = beatEpochMs + nextBeatIndex * beatPeriodMs;
      const msUntilBeat = beatServerTime - now;
      if (msUntilBeat > LOOKAHEAD_MS) break;

      const audioTime = audioCtx.currentTime + msUntilBeat / 1000;
      // All 3 channels are scheduled every beat ("in lockstep"); only the
      // selected channel's gain node is actually audible.
      for (let ch = 0; ch < CHANNELS.length; ch++) {
        playClick(ch, audioTime);
      }
      if (msUntilBeat >= 0) {
        setTimeout(() => pulseVisual(PULSE_WIDTH_MS), Math.max(0, msUntilBeat));
      }
      nextBeatIndex++;
    }

    requestAnimationFrame(schedulerTick);
  }

  channelBtns.forEach((btn) => {
    btn.addEventListener('click', () => selectChannel(Number(btn.dataset.channel)));
  });

  joinBtn.addEventListener('click', async () => {
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      channelGains = CHANNELS.map(() => {
        const g = audioCtx.createGain();
        g.gain.value = 0;
        g.connect(audioCtx.destination);
        return g;
      });
    }
    await audioCtx.resume();

    channelGains[selectedChannel].gain.value = CHANNEL_GAIN;
    document.body.dataset.channel = String(selectedChannel);
    channelBtns[selectedChannel].classList.add('selected');
    statChannel.textContent = CHANNELS[selectedChannel].name;
    channelsEl.hidden = false;

    playing = true;
    nextBeatIndex = null;
    joinBtn.hidden = true;
    setStatus('Playing.');
    requestAnimationFrame(schedulerTick);
  });

  connect();
})();
