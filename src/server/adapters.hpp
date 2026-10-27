#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "servers.hpp"

namespace aio {

// An adapter file (<config_dir>/adapters/<name>.json) teaches aio how to connect one
// CLI to aio MCP servers, so new CLIs need no rebuild. Format:
//
//   match            ["claude"]            first word of the agent command (required)
//   config_file      {name?, content}      JSON written to aio's state dir; path is {config_file}
//   project_file     {path, content}       JSON merged into <agent dir>/<path> (relative)
//   args             ["--x", "{...}"]      appended once
//   args_per_server  [...]                 appended once per server
//   env, env_per_server  {"K": "V"}        environment for the agent process
//   insert_args      "end" | "after_command"
//
// Placeholders: {agent} {config_file} {tools_csv} everywhere; per server (inside
// *_per_server and "{each_server}" object keys, which expand to one entry per
// server named {mcp_name}): {server} {mcp_name} {url} {port} {token} {token_env}.
struct Adapter {
    std::string name;  // file name without .json
    std::vector<std::string> match;
    nlohmann::json spec;
};

struct AdapterSet {
    std::string dir;
    std::vector<Adapter> adapters;
    std::vector<std::string> errors;  // "<file>: <problem>" for files that were skipped
};

std::string adapters_dir();

// Built-in adapters (name, file contents) written when the adapters dir is first created.
std::vector<std::pair<std::string, std::string>> builtin_adapters();

// Loads every *.json in `dir`, seeding the built-ins if the directory does not exist yet.
AdapterSet load_adapters(const std::string& dir = adapters_dir());

// Validates one adapter document; returns an error or "".
std::string validate_adapter(const nlohmann::json& spec);

// First word of a command without any directory prefix ("/usr/bin/claude -c" -> "claude").
std::string command_word(const std::string& command);

const Adapter* find_adapter(const AdapterSet& set, const std::string& command);

struct LaunchWiring {
    std::string command;
    std::vector<std::pair<std::string, std::string>> env;
    bool wired = false;  // an adapter matched and was applied
    std::string error;
};

struct ServerMembership {
    ServerInfo server;
    std::string token;
};

// Where generated agent config and trace records live: <state_dir>/agents.
std::string agent_state_dir();

// Rewrites an agent launch so its CLI connects to `servers`, using the matching adapter.
// `state_dir` receives generated config files; `work_dir` is the agent's directory.
// Everything written is recorded in <state_dir>/<agent>.traces.json for remove_agent_traces.
LaunchWiring wire_messaging(const AdapterSet& adapters, const std::string& agent,
                            const std::string& command, const std::string& work_dir,
                            const std::vector<ServerMembership>& servers, const std::string& state_dir);

// Undoes what wire_messaging wrote for `agent`: deletes generated config files and removes
// its server entries from project files (deleting files/dirs aio created once empty).
// Entries another live agent still relies on are kept.
void remove_agent_traces(const std::string& agent, const std::string& state_dir = agent_state_dir());

// Agents that still have a trace record.
std::vector<std::string> traced_agents(const std::string& state_dir = agent_state_dir());

}  // namespace aio
