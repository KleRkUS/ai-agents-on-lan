#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "servers.hpp"

namespace aio {

// Everything a request needs to know about who is calling and where messages live.
struct McpContext {
    std::string server;
    std::string agent;
    std::string mail_root;
    MessagingConfig limits;
    std::vector<std::string> members;  // every agent on this server, including `agent`
    bool manager = false;              // this caller may stop, kill and create agents
};

// Handles one JSON-RPC message; returns the response, or null for notifications.
nlohmann::json handle_mcp_request(const McpContext& ctx, const nlohmann::json& request);

struct HttpReply {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
};

// Streamable-HTTP MCP endpoint logic, independent of the socket layer.
HttpReply handle_mcp_http(const ServerRegistry& registry, const std::string& server,
                          const MessagingConfig& limits, const std::string& authorization,
                          const std::string& body);

// Runs `aio serve --name <server>` until killed. Returns a process exit code.
int run_server(const std::string& server);

}  // namespace aio
