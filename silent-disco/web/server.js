// Silent Disco web app -- step 2 per silent-disco/CLAUDE.md's build order.
// Master clock + WebSocket clock sync for phones; see docs/web-sync-protocol.md.

const http = require('http');
const path = require('path');
const express = require('express');
const { WebSocketServer } = require('ws');

const PORT = process.env.PORT || 3000;

// Step 2: single fixed channel/tempo, matching beacon/main/ble_beacon.c's
// CHANNEL_ID/BPM. Step 4 makes these genuinely variable.
const CHANNEL_ID = 0;
const BPM = 120;
const BEAT_EPOCH_MS = Date.now(); // fixed anchor, "beat 0" -- see docs/web-sync-protocol.md's known gap

const app = express();
app.use(express.static(path.join(__dirname, 'public')));

const server = http.createServer(app);
const wss = new WebSocketServer({ server });

wss.on('connection', (ws) => {
  ws.send(JSON.stringify({
    type: 'hello',
    channelId: CHANNEL_ID,
    bpm: BPM,
    beatEpochMs: BEAT_EPOCH_MS,
  }));

  ws.on('message', (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    if (msg.type === 'ping' && typeof msg.t0 === 'number') {
      ws.send(JSON.stringify({ type: 'pong', t0: msg.t0, t1: Date.now() }));
    }
  });
});

server.listen(PORT, () => {
  console.log(`Silent Disco web app listening on :${PORT} (channel ${CHANNEL_ID}, ${BPM} BPM)`);
});
