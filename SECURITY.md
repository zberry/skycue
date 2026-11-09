# Security

## Supported versions

Only the latest release is supported. Today that is 0.1.x.

## Reporting a vulnerability

Use GitHub's private route: on the repository's Security tab, choose
"Report a vulnerability". Please do not open a public issue for a security
problem.

## The web UI and API have no authentication

Anyone who can reach `web.bind`:`web.port` can read live data and can change
filters, allow and block lists, inputs, outputs and the configuration. The
default in `packaging/skycue.toml.example` is `bind = "127.0.0.1"` and
`port = 8080`, so only the machine itself can reach the page. Set `web.bind`
to another address only on a network you trust, or put a proxy that
authenticates in front of it.

## Restart

`web.allow_restart` lets anyone who can reach the web port restart the
daemon. It is `false` in `packaging/skycue.toml.example`, so a source build
run with that file does not allow it. The packages install `/etc/skycue/skycue.toml`
with `web.allow_restart = true` (set by `cmake/PackageConfig.cmake`), because the
systemd unit starts skycued again after the restart. With a package, anyone
who can reach the web port can restart the daemon unless you set it back
to `false`.

## Same-origin and content-type checks

A request that changes state (`POST`, `PUT`, `DELETE`) is checked before it is
acted on:

- Unless it is a `DELETE`, it must have `Content-Type: application/json`;
  otherwise the answer is 415.
- If it has an `Origin` header, the origin must be the same host and port as the
  `Host` header; otherwise the answer is 403.

These checks stop a web page on another site from driving the API through a
visitor's browser. They do not stop a client on the network that sends the
right headers.

## What is out of scope

The cue stream and the raw outputs (Beast, dump978 lines, readsb JSON,
pingStation texts, CAT021) are plain TCP or UDP without authentication or
encryption, by design. Limit who can reach those ports with the network.
