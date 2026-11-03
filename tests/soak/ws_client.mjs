// A soak WebSocket client (tests/soak/soak.sh): node ws_client.mjs URL
//
// Connects to a skycued /ws, counts the messages, reconnects (after 1 s) when
// the socket closes or fails, and logs each reconnect to stderr. On SIGTERM
// or SIGINT it prints the totals to stderr and exits 0. Needs node 22 (the
// built-in WebSocket); no npm packages.

const url = process.argv[2];
if (!url) {
  console.error('usage: node ws_client.mjs ws://HOST:PORT/ws');
  process.exit(2);
}
if (typeof WebSocket !== 'function') {
  console.error('ws_client: this node has no built-in WebSocket (node 22 or later is needed)');
  process.exit(2);
}

let messages = 0;
let bytes = 0;
let connects = 0;
let reconnectTimer = null;

function stamp() {
  return new Date().toISOString();
}

function connect() {
  reconnectTimer = null;
  const ws = new WebSocket(url);
  let closed = false;
  ws.addEventListener('open', () => {
    connects += 1;
    if (connects > 1)
      console.error(`${stamp()} ws_client: reconnected to ${url} (connect ${connects}, ${messages} messages so far)`);
  });
  ws.addEventListener('message', (ev) => {
    messages += 1;
    const d = ev.data;
    bytes += typeof d === 'string' ? d.length : (d && d.byteLength) || 0;
  });
  const retry = (why) => {
    if (closed)
      return;
    closed = true;
    console.error(`${stamp()} ws_client: ${why}; reconnecting in 1 s`);
    if (!reconnectTimer)
      reconnectTimer = setTimeout(connect, 1000);
  };
  ws.addEventListener('close', (ev) => retry(`closed (code ${ev.code})`));
  ws.addEventListener('error', () => retry('error'));
}

function finish() {
  console.error(`${stamp()} ws_client: ${messages} messages, ${bytes} bytes, ${connects} connects`);
  process.exit(0);
}
process.on('SIGTERM', finish);
process.on('SIGINT', finish);

connect();
