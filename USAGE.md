# Using aio

aio runs several AI CLI agents from one terminal. A list sits on the left. The agent you open fills the right side. Agents can message each other through local MCP servers. Agents stay on the machine where you start them. That machine needs Linux or macOS, tmux, and the agent CLIs on `PATH` (`claude`, `agent`, `codex`, or anything else). Another device reaches them through a relay, which does not run the agents itself.

## Build

You need CMake 3.20 or newer, a C++20 compiler, and git. The first configure downloads FTXUI, nlohmann/json, and cpp-httplib.

```sh
cmake -S . -B build
cmake --build build
./build/aio --version
```

The binary is `build/aio`. Platform is chosen at configure time and defaults to the system you compile on. To cross-check the choice: `-DAIO_PLATFORM=linux`, `macos`, or `windows`.

Tests use a private tmux socket and temporary config directories. They do not touch a running aio session.

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

## Start and stop

```sh
./build/aio
```

The first launch creates a token and a detached control process on `127.0.0.1:47710`, then opens a tmux window on a private socket named `aio` (`tmux -L aio`). That socket is not your default tmux server. The control port is not reachable from other devices. Run aio again to reattach. Agents keep running if you close the terminal without quitting.

Press `q` in the list and confirm to quit. On the machine that owns the agents this stops every agent and MCP server, removes the config files aio wrote for those agents, and restores the terminal. Settings and server definitions stay.

If aio says an older control server is still on port 47710, stop it and run aio again. A previous build listened on port 47700; stop that process before starting a relay, which uses 47700.

## Layout

| Key | List |
| --- | --- |
| `j` / `k` or arrows | Move |
| `1` / `2` | Agents tab / MCP servers tab |
| Enter or `l` | Open the agent, or the server log |
| `n` | New agent or new server |
| `d` | Kill the agent, or delete the server |
| Space | Start or stop the selected server |
| `s` | Settings |
| `q` | Quit, after a confirmation |

`C-x` moves focus back to the list. It works while you are typing in the agent. Change it in settings if you need to; the sidebar reserves `j k l n q s 1 2`.

A green dot marks the agent currently on the right. A dead agent stays in the list with its last output until you remove it. An agent's outline uses the color of an MCP server it belongs to. Agents with no server stay gray.

Popups zoom the list over the agent. Closing the popup restores the split.

## Agents

On the agents tab, press `n`. Pick a preset or type a command, set the working directory (`~` is fine), and optionally mark the agent as a manager and tick the servers it should join. The directory must already exist. The name is filled in from the preset; you can change it. Names are 1–32 characters from `A-Z`, `a-z`, `0-9`, `.`, `_`, `-`, and cannot start with `.`.

Presets shipped by default:

| Name | Command |
| --- | --- |
| claude | `claude` |
| claude resume | `claude --resume` |
| claude continue | `claude --continue` |
| cursor | `agent` |
| cursor resume | `agent resume` |
| codex | `codex` |

The Cursor CLI on `PATH` is `agent` (or `cursor-agent`). aio does not launch a command named `cursor`.

Only agents created while messaging is on, and assigned to a server, are wired to that server. An agent that is already running cannot be attached to a server later. Edit or add presets in settings; a preset name cannot contain `|` or a newline.

`d` stops the agent and deletes the files aio generated for it. The project files you already had are left in place, except MCP entries aio itself merged in.

## MCP servers and messages

Servers are local HTTP processes. They listen on `127.0.0.1` only, starting at port 47800. They are not reachable from another device. The relay token does not open these ports.

On tab 2, press `n` and name the server. Names are 1–24 characters from `a-z`, `0-9`, and `-`, and cannot start or end with `-`. Space starts and stops it. Enter shows its log on the right. `d` deletes the definition.

Create the server first, then create agents and assign them to it. Each server gets a color. Members of a server can use these tools:

- `send_message` — one call, one or more recipients. The same text is delivered to all of them, with `From` and `To` filled in.
- `read_messages` — only this agent's unread mail. Use it when the typed notice was cut off.
- `list_agents` — who is on this server.

aio does not interrupt an agent that is working. When the terminal has been idle (3 seconds by default), aio types the message into the prompt, prefixed with `[aio]`. The agent should answer with `send_message` only when a reply is needed.

Limits, all editable in settings: 4000 characters, 10 messages per minute per sender, a reply chain of 6 hops, 50 unread messages per inbox. Turn messaging off to stop new wiring and refuse sends. Existing servers stay until you stop them.

## Managers

Tick "Manager" when creating an agent. A manager is a normal member of its servers, plus three tools workers do not have:

- `stop_agent` sends Ctrl+C. The agent stays open and can still receive mail.
- `kill_agent` closes another agent and removes aio's files for it.
- `create_agent` starts a worker. It cannot create another manager.

A manager cannot stop, kill, or target itself, and cannot stop or kill another manager.

## Relay

The relay is a separate process. It listens on `0.0.0.0:47700` and keeps a list of machines that have joined. It does not start tmux, agents, or MCP. Run it on a machine the others can reach (that can be the same computer):

```sh
aio --relay
```

The first run writes `access.token` (64 hex characters, mode `0600`) in the config directory. Every connection has to send it. On the machine that owns the agents:

```sh
aio --join ADDRESS --token TOKEN
```

`ADDRESS` is the relay, and `TOKEN` is the relay's token, not a second secret. The command starts the local agent host if it is not already up, then holds the registration open. Settings on that machine shows its host id and the join command. Settings on a viewer names the machine it is controlling. Closing the join process drops the machine from the relay's list. The agents keep running.

On another computer:

```sh
aio --host ADDRESS --token TOKEN
```

That draws the owning machine's agents. The relay forwards list, create, kill, stop, settings, MCP start/stop, and the live terminal. The sidebar title is the machine being driven. When several machines have joined, the list asks which one; `m` opens that list again and Enter selects. A machine you are already driving stays selected when another one joins. With one machine joined, it is selected for you. Show, zoom, and shutdown from the other computer do not touch the owning machine. Quit there disconnects only that computer. Guest Wi-Fi that isolates clients blocks the connection. `AIO_ACCESS_TOKEN` on the relay overrides its token file.

## Adapters

aio learns how to attach a CLI to MCP servers from JSON files, not from a rebuild. They live in the adapters directory and are created on first launch for Claude, Codex, and Cursor. Settings shows which files loaded and any that failed to parse. Edit them while settings is closed; they reload when you open settings.

A file matches the first word of the agent command (`claude`, `agent`, `cursor-agent`, `codex`). Useful fields:

- `match` — command names
- `args` — appended once
- `args_per_server` — appended once per server
- `env`, `env_per_server` — environment for the agent process
- `config_file` — JSON aio writes under its state directory
- `project_file` — JSON merged into the agent's directory (Cursor uses `.cursor/mcp.json`)
- `insert_args` — `end` (default) or `after_command`

Placeholders: `{agent}`, `{config_file}`, `{tools_csv}`, and per server `{server}`, `{mcp_name}`, `{url}`, `{port}`, `{token}`, `{token_env}`. A key `{each_server}` expands to one object entry per server.

Delete an adapter file to stop wiring that CLI. aio records what it wrote and removes those traces when the agent is killed.

## Files

Linux:

| Path | Contents |
| --- | --- |
| `~/.config/aio/config` | Settings (`key=value` lines) |
| `~/.config/aio/access.token` | Token for this machine's control port, and for the relay if the relay runs as the same user |
| `~/.config/aio/host.id` | Stable id of this machine on the relay |
| `~/.config/aio/adapters/*.json` | CLI adapters |
| `~/.local/state/aio/aio.log` | Event log, rotated past 1 MB |
| `~/.local/state/aio/servers/` | Server definitions and mailboxes |
| `~/.local/state/aio/agents/` | Generated agent config and trace records |

`XDG_CONFIG_HOME` and `XDG_STATE_HOME` replace `~/.config` and `~/.local/state`.

macOS uses `~/Library/Application Support/aio` for config, with state under `state/` inside that directory.

The log is the place to look when an agent or server fails to start. Lines are timestamped events (created, exited, manager actions), not the agent's own output. The agent's output stays in its pane.

## Release builds

GitHub Actions builds and tests pull requests on Linux (gcc and clang) and macOS. The Release workflow is manual, from `main`, and publishes `aio-<version>-linux-x86_64`, `linux-arm64`, `macos-arm64`, and `macos-x86_64`. The version is `project(... VERSION ...)` in `CMakeLists.txt`. Bump it before a release; the tag `v<version>` must not already exist.
