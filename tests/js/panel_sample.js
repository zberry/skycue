'use strict';
// The fixed snapshot the Inputs / Outputs card markup test renders: a
// clock, input and output lists and the page state (rates, decoder lines,
// data times) the input cards read.

var NOW = Date.parse('2026-01-02T03:04:05Z');

var inputs = [
  { id: 'readsb', type: 'readsb', builtin: true, enabled: true, transport: 'tcp_connect', host: '127.0.0.1', port: 30005,
    state: { connected: true, messages: 40, observations: 40, rejected: 3, last_data_age_s: 2 } },
  { id: 'ps-roof', name: 'Roof <b>pi</b>', type: 'pingstation_json', enabled: true, transport: 'udp', port: 4000,
    status_interval_s: 30,
    state: { connected: true, messages: 5, observations: 5, last_data_age_s: 100, last_error: 'bad frame',
      device_status: { lat: 38.5, lon: -77.5, gps_status: 1, receiver_status: 0, version: '1.2.3',
        received_at: new Date(NOW - 20000).toISOString() } } },
  { id: 'ps-new', type: 'pingstation_cat021', enabled: false, transport: 'udp', port: 4001, state: {} },
  { id: 'uat1', type: 'dump978', enabled: true, transport: 'tcp_connect', host: 'localhost', port: 30978,
    state: { connected: false, messages: 0, observations: 0 } }
];
var outputs = [
  { id: 'beast-out', kind: 'beast', builtin: true, enabled: true, transport: 'tcp_listen', bind: '0.0.0.0', port: 30005,
    state: { up: true, peers: 2, bytes: 1234, dropped: 1 } },
  { id: 'feed-1', name: 'Feed <i>1</i>', kind: 'sbs', enabled: true, transport: 'tcp_connect', host: 'feed.example', port: 30003,
    state: { up: false, last_error: 'connection refused' } },
  { id: 'off', kind: 'json', enabled: false, transport: 'udp', host: '10.0.0.9', port: 4444, state: {} }
];
var page = {
  wall: NOW - 1000,
  rates: { readsb: { obs_s: 12.5, msg_s: 13.25 }, 'ps-roof': { obs_s: 0, msg_s: 0 } },
  decoderLines: { 'ps-roof': 'decoded 5 · skipped 0' },
  dataWall: { readsb: NOW - 500 }
};

module.exports = { NOW: NOW, inputs: inputs, outputs: outputs, page: page };
