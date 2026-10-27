# Integrating with aio

This is the contract for an application that drives agents on another machine. The official desktop client is documented in [USAGE.md](USAGE.md). A new client speaks HTTP and WebSocket to the relay. It does not link the `aio` binary, and it does not run agents.

Protocol version is **3**. `GET /v1/health` returns it. Stop if the number is different.

## Roles

Three processes cooperate. Only the first two are the `aio` binary. A new app is the third.

| Role | Program | What it does |
| --- | --- | --- |
| Agent host | `aio --control`, then `aio --join` | Linux or macOS. Owns tmux, the agent CLIs, and the MCP servers. Listens on `127.0.0.1:47710`, which is not reachable from other devices. |
| Relay | `aio --relay` | Listens on `0.0.0.0:47700`. Forwards control calls and terminal bytes to a joined host. Does not start agents. |
| Controller | your app, or `aio --host` | Lists machines, creates and stops agents, and draws a terminal. Runs nowhere near the agents. |

Android, and a future Windows full-screen client, are controllers. They do not join the relay as a host, and they do not open port 47710 or the MCP ports. Those ports stay on the machine that runs the agents.

The relay's join socket (`/v1/join`, with `op` frames and an 8-byte stream id) is how the agent host registers. A controller never opens it.

## Connect

```http
Authorization: Bearer TOKEN
```

`TOKEN` is the relay's `access.token` (64 hex characters). A missing or wrong token is `401` and `{"error":"unauthorized"}` on HTTP and on the websocket upgrade, before the protocol switches.

Check the relay, then list machines:

```http
GET /v1/health
GET /v1/hosts
```

`/v1/health` on the relay is `{"ok":true,"role":"relay","protocol":3}`. Require `role` to be `relay`. The agent host's health object has no `role`.

`/v1/hosts` is `{"hosts":[{"id":"...","name":"..."}]}`, sorted by name. `id` is 1–64 characters from `[A-Za-z0-9._-]`. Remember it. It is the stable id of that machine.

| Joined hosts | What the controller does |
| --- | --- |
| 0 | Nothing to drive. `GET /v1/state` is `503` `no agent host is joined`. |
| 1 | `/v1/state` and the other short paths work. Prefer `/v1/hosts/<id>/...` anyway, so a second machine joining does not move the session. |
| 2 or more | Short paths are `400` `more than one host is joined`. Every later call uses `/v1/hosts/<id>/...`. |

The user picks the machine. Keep that id when the list changes. Drop it only when that id disappears from `/v1/hosts`.

A short path `/v1/state` and the prefixed path `/v1/hosts/<id>/state` are the same request once a host is chosen. The prefix is `/v1/hosts/<id>` plus the path that begins with `/v1`.

The relay waits up to 20 seconds for the host. After that the status is `504`. If the host drops, in-flight calls return `503`.

## Calls a controller makes

Send these to the relay. JSON bodies are objects. A successful change is usually `200` and `{"error":""}`. A failure is `{"error":"..."}` with an HTTP status of 300 or more.

| Method and path | Body | Effect on the agent host |
| --- | --- | --- |
| `GET /v1/state` | | Agents, MCP servers, settings, host name |
| `GET /v1/log` | | Last part of the host's event log |
| `PUT /v1/config` | settings object | Saves settings |
| `POST /v1/agents` | see below | Creates an agent |
| `DELETE /v1/agents/<name>` | | Kills that agent and removes aio's files for it |
| `POST /v1/agents/<name>/stop` | `{}` | Sends Ctrl+C. The agent stays in the list |
| `POST /v1/servers` | `{"name","port"}` | Creates and starts an MCP server. Use `next_port` from state. The port must be 1024–65535 |
| `POST /v1/servers/<name>/start` | `{}` | Starts it |
| `POST /v1/servers/<name>/stop` | `{}` | Stops it |
| `DELETE /v1/servers/<name>` | | Deletes the definition |
| `WebSocket /v1/terminal/agents/<name>` | | Live terminal |
| `WebSocket /v1/terminal/servers/<name>` | | Live MCP log |

Names in the path are a single segment. Reject a name that contains `/` before you send it.

Create an agent:

```json
{"name":"claude-1","directory":"/home/me/project","command":"claude","servers":["team"],"manager":false}
```

`name`, `directory`, and `command` are required. `directory` is a path on the **agent host**, and it must already exist. Do not send a path from the phone or from the controller's disk. The desktop client expands `~` before it sends; the relay does not. `servers` names MCP servers that already exist on that host. `manager` defaults to false.

`POST .../show`, `POST /v1/layout/zoom`, and `POST /v1/shutdown` are not forwarded. Show and zoom come back `200` with `{"error":""}` and do not move panes on the owner. Shutdown is `403`. A controller draws the terminal itself and exits locally. It does not shut the relay or the agents down.

## State

`GET /v1/state` is one JSON object. Poll it. The fields a controller uses:

```json
{
  "host": {"id": "...", "name": "workstation"},
  "viewport": "",
  "next_port": 47800,
  "agents": [
    {"name": "claude-1", "dir": "/home/me/project", "command": "claude",
     "dead": false, "exit_status": 0, "manager": false,
     "servers": ["team"], "unread": 0}
  ],
  "servers": [
    {"name": "team", "port": 47800, "color": "#4fc3f7", "running": true,
     "error": "", "unread": 0, "members": ["claude-1"]}
  ],
  "config": {},
  "adapters": {"dir": "...", "adapters": [{"name": "claude", "match": ["claude"]}], "errors": []}
}
```

`host.id` matches the id from `/v1/hosts`. `host.name` is the title to show.

`viewport` describes the owner's own sidebar, not the controller. Leave it. A phone remembers which terminal it opened.

`config` is the settings object accepted by `PUT /v1/config`. Send back the fields you change. Presets are `{"name","command"}`. Messaging fields are `enabled`, `auto_deliver`, `max_message_chars`, `max_messages_per_minute`, `max_hops`, `max_inbox`, and `idle_seconds`.

`agents[].pane_id` and `access` are host internals. `access.token`, `access.port`, and `access.addresses` refer to `127.0.0.1:47710` and the host's other interfaces. Do not connect to them. The relay token is the only credential the controller holds.

MCP servers listen on `127.0.0.1` of the agent host, from port 47800. The relay token does not open them. Agents message each other through those servers. A controller shows unread counts from `state`. It does not deliver mail.

A dead agent stays in the list until `DELETE`. `exit_status` is the code it exited with.

## Terminal

Open `ws://RELAY:47700/v1/hosts/<id>/terminal/agents/<name>` (or `servers`) with the same `Authorization` header. The upgrade is rejected with `401` when the token is wrong.

The controller's websocket carries raw terminal bytes. It does not use the 8-byte stream id. That prefix exists only on the host's join socket.

| Direction | Frame | Meaning |
| --- | --- | --- |
| Controller to relay | text `{"cols":N,"rows":M}` | Terminal size. Send this first, and again on resize. |
| Controller to relay | binary | Keystrokes, as bytes, in order. |
| Relay to controller | binary | Output, including the initial screen and ANSI sequences. |
| Relay to controller | text `{"error":"..."}` | The stream failed. Close it. |

The host waits about a second for the first size frame, then sends a snapshot. A later size frame still applies. Do not put a short read timeout in the middle of a websocket frame. Wait for a whole frame, or block.

Closing the socket leaves the agent running. `DELETE` and `stop` are separate HTTP calls.

There is no TLS in protocol 3. Android refuses cleartext `http://` and `ws://` unless the app allows it for this host. A controller on a network that isolates clients cannot reach the relay. The relay is the address the user configured, not an address discovered from `state.access`.

## What each platform builds

| Platform | Build |
| --- | --- |
| Linux, macOS | The `aio` binary. It can be the agent host, the relay, or a controller (`aio --host`). The controller still uses tmux for its own window. |
| Windows | A controller only, when one is written. It does not ship tmux and does not run agents. The wire protocol is the one above. |
| Android | A controller only. Kotlin or Java, in this repo or another. One activity can list hosts, list agents, and draw the terminal. It stores the relay address and token, pins a host id, and follows the tables above. |

A controller that only implements `GET /v1/health`, `GET /v1/hosts`, `GET /v1/state`, create, delete, stop, and the terminal websocket already cooperates with the machines that have joined.
