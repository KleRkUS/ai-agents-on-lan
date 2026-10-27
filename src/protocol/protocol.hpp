#pragma once

// The contract between the Linux server and every client.
// Wire messages will be added here. Nothing in this folder calls the OS.

namespace aio {

// Bumped when a client can no longer talk to an older server.
inline constexpr int kProtocolVersion = 3;

// Agent host. Loopback only: agents stay on this machine.
inline constexpr const char* kHostBind = "127.0.0.1";
inline constexpr int kHostPort = 47710;

// Relay. Joined hosts and other devices connect here. Every request needs the token.
inline constexpr const char* kRelayBind = "0.0.0.0";
inline constexpr int kRelayPort = 47700;

// Local clients talk to the agent host. These names are the host address.
inline constexpr const char* kControlHost = kHostBind;
inline constexpr const char* kControlBind = kHostBind;
inline constexpr int kControlPort = kHostPort;

// Live terminal, on the agent host. Binary websocket frames are raw terminal bytes,
// in both directions. A client text frame is {"cols":N,"rows":M}. A server text
// frame is {"error":"..."}. Paths: /v1/terminal/agents/<name> and /v1/terminal/servers/<name>.
//
// Relay registration. GET /v1/hosts lists joined machines. A host holds open
// /v1/join. Its first text frame is {"id","name"}. The relay answers {"ok":true}.
// Later frames: host {"op":"ping"}; relay {"op":"call","id","method","path","body"};
// host {"op":"result","id","status","body"}.
// Terminal bytes use the same socket. Relay {"op":"open","id","path"} and
// {"op":"text","id","body"} and {"op":"close","id"}. Host {"op":"opened","id","ok"}
// and {"op":"text","id","body"} and {"op":"close","id"}. Binary frames are
// 8 big-endian id bytes plus the raw terminal bytes.
// With one joined host, /v1/state, /v1/terminal/..., and the other control paths
// go to it. /v1/hosts/<id>/... selects a host. A viewer with several hosts uses the
// prefixed paths. Show, zoom, and shutdown do not.

}  // namespace aio
