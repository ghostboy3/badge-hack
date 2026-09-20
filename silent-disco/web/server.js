// Silent Disco web app -- master clock + WebSocket clock sync for phones
// (docs/web-sync-protocol.md), and a real-playlist-per-channel scan on
// startup instead of a synthesized beat click (see web/public/audio/*/README.md
// for how to add tracks).

const fs = require('fs');
const http = require('http');
const path = require('path');
const express = require('express');
const { WebSocketServer } = require('ws');

const PORT = process.env.PORT || 3000;

// Index order matches client.js's CHANNELS array (Pink/Orange/Purple).
const CHANNEL_DIRS = ['pink', 'orange', 'purple'];
const AUDIO_EXTENSIONS = new Set(['.mp3', '.ogg', '.wav', '.m4a']);
const AUDIO_ROOT = path.join(__dirname, 'public', 'audio');

const PLAYLIST_EPOCH_MS = Date.now(); // fixed anchor -- see docs/web-sync-protocol.md's known gap

function titleFromFilename(file) {
  return path.basename(file, path.extname(file)).replace(/^\d+[-_.\s]*/, '');
}

async function loadChannelPlaylist(dir, parseFile) {
  let files;
  try {
    files = fs.readdirSync(path.join(AUDIO_ROOT, dir));
  } catch {
    return []; // folder doesn't exist yet -- not an error, just no tracks
  }

  files = files
    .filter((f) => AUDIO_EXTENSIONS.has(path.extname(f).toLowerCase()))
    .sort(); // filename prefixes (01-, 02-, ...) control playback order

  const playlist = [];
  for (const file of files) {
    const fullPath = path.join(AUDIO_ROOT, dir, file);
    let durationSec = 0;
    let title = titleFromFilename(file);
    try {
      const meta = await parseFile(fullPath);
      if (meta.format.duration) durationSec = meta.format.duration;
      if (meta.common.title) title = meta.common.title;
    } catch (err) {
      console.error(`skipping ${fullPath}: ${err.message}`);
      continue;
    }
    if (durationSec <= 0) {
      console.error(`skipping ${fullPath}: could not determine duration`);
      continue;
    }
    playlist.push({ url: `/audio/${dir}/${file}`, title, durationSec });
  }
  return playlist;
}

async function loadChannels() {
  const { parseFile } = await import('music-metadata'); // ESM-only package, CJS server
  const channels = [];
  for (const dir of CHANNEL_DIRS) {
    channels.push({ playlist: await loadChannelPlaylist(dir, parseFile) });
  }
  return channels;
}

async function main() {
  const channels = await loadChannels();
  channels.forEach((ch, i) => {
    console.log(`channel ${i} (${CHANNEL_DIRS[i]}): ${ch.playlist.length} track(s)`);
  });

  const app = express();
  app.use(express.static(path.join(__dirname, 'public')));

  const server = http.createServer(app);
  const wss = new WebSocketServer({ server });

  wss.on('connection', (ws) => {
    ws.send(JSON.stringify({
      type: 'hello',
      playlistEpochMs: PLAYLIST_EPOCH_MS,
      channels,
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
    console.log(`Silent Disco web app listening on :${PORT}`);
  });
}

main();
