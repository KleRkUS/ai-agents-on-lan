#include "mcp.hpp"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <mutex>

#include <httplib.h>

#include "agents.hpp"
#include "log.hpp"
#include "mailbox.hpp"
#include "platform.hpp"

namespace aio {

using json = nlohmann::json;

namespace {

constexpr const char* kProtocolVersion = "2025-06-18";

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

json tool_text(const std::string& text, bool is_error = false) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", is_error}};
}

// Handed to the CLI as MCP server instructions when the agent connects, so it knows the
// protocol before it ever calls a tool.
std::string messaging_guide(const McpContext& ctx) {
    std::string others;
    for (const auto& m : ctx.members)
        if (m != ctx.agent) others += (others.empty() ? "" : ", ") + m;
    if (others.empty()) others = "(nobody else yet)";

    return "You are agent '" + ctx.agent + "' on the aio server '" + ctx.server +
           "'. This is the whole messaging protocol. Do not explore the tools to figure it out.\n"
           "\n"
           "Who is who\n"
           "- Your name is " + ctx.agent + ". A message is for you exactly when it says To: you (" +
           ctx.agent + "). read_messages never returns messages for anyone else, so you do not "
           "have to work out which ones are yours.\n"
           "- Other agents on this server right now: " + others + ".\n"
           "- Every message names its sender (From) and every recipient (To: you, plus Also sent "
           "to). A message with several recipients was delivered to all of them at the same time, "
           "each seeing the same sender and the same recipient list.\n"
           "\n"
           "Sending\n"
           "- send_message takes \"to\" (one name, or a list of names) and \"message\".\n"
           "- One call delivers the same message to every name in \"to\" at once. A manager "
           "messaging two workers sends \"to\": [\"agent-2\", \"agent-3\"] a single time, not one "
           "call per worker.\n"
           "- Write the message so it stands alone. Recipients cannot see your conversation. Say "
           "what you want from each of them. Do not message yourself.\n"
           "\n"
           "Receiving\n"
           "- You do not poll and you do not stop working. While your terminal is busy, aio holds "
           "the message and types nothing.\n"
           "- When your terminal has been quiet, aio types the message into your prompt, starting "
           "with [aio]. That typed text IS the message: From, To, and the body. Finish anything "
           "still in progress, then act on it. Reply with send_message only if a reply is needed.\n"
           "- read_messages returns those same messages again and marks them read. Call it only "
           "when the typed text was cut off. list_agents shows your name and the others.";
}

std::string manager_guide(const McpContext& ctx) {
    if (!ctx.manager) return {};
    return "\n\nManaging other agents\n"
           "You are a manager. Workers do not have these tools.\n"
           "- stop_agent sends Ctrl+C to an agent's pane. That cancels its current turn and leaves "
           "the agent running, so it can still receive a message afterwards. It does not close the "
           "agent.\n"
           "- kill_agent closes an agent and removes the config aio wrote for it.\n"
           "- create_agent starts a worker. Pass name, directory, and command. servers is an "
           "optional list of aio server names for it to join. You cannot create a manager.\n"
           "- You cannot stop or kill yourself, and you cannot stop or kill another manager. "
           "list_agents marks managers.\n"
           "- Stop someone who is stuck or going the wrong way. Kill only when the agent itself "
           "should go away. Create a replacement worker afterwards if the work should continue.";
}

json tool_definitions(const McpContext& ctx) {
    json tools = json::array({
        {
            {"name", "list_agents"},
            {"description",
             "Your own name and the other agents on this server. Pass their names as \"to\" to "
             "send_message. You already know this from the server instructions; call it only to "
             "refresh the list."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
        },
        {
            {"name", "send_message"},
            {"description",
             "Deliver one message to one or more agents on this server at the same time. \"to\" is "
             "a name or a list of names; every recipient sees you as From and sees the full "
             "recipient list. The message must stand alone. Recipients are not interrupted: aio "
             "types it into their prompt only once they are idle."},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"to",
                 {{"description",
                   "Recipient name, or a list of names to deliver the same message to all of them "
                   "at once. Example: [\"agent-2\", \"agent-3\"]."},
                  {"oneOf", json::array({{{"type", "string"}},
                                         {{"type", "array"}, {"items", {{"type", "string"}}}}})}}},
                {"message", {{"type", "string"}, {"description", "The message text. Include who you are and what you want."}}}}},
              {"required", json::array({"to", "message"})}}},
        },
        {
            {"name", "read_messages"},
            {"description",
             "Returns the messages sent to you and marks them read. Each one says From (the "
             "sender) and To: you, plus anyone else who got the same message. You usually already "
             "have this text typed into your prompt; call this only if that text was cut off."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
        },
    });
    if (!ctx.manager) return tools;
    auto schema = [](json properties, json required) {
        return json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
    };
    tools.push_back({{"name", "stop_agent"},
                     {"description",
                      "Cancel another agent's current turn by sending Ctrl+C. The agent keeps running and can "
                      "receive messages afterwards. You cannot stop yourself or another manager."},
                     {"inputSchema", schema({{"name", {{"type", "string"}, {"description", "Agent to stop"}}}},
                                            json::array({"name"}))}});
    tools.push_back({{"name", "kill_agent"},
                     {"description",
                      "Close another agent and remove the config aio wrote for it. You cannot kill yourself "
                      "or another manager."},
                     {"inputSchema", schema({{"name", {{"type", "string"}, {"description", "Agent to kill"}}}},
                                            json::array({"name"}))}});
    tools.push_back(
        {{"name", "create_agent"},
         {"description",
          "Start a new worker agent in the background. It is never a manager. directory must already exist. "
          "command is the CLI to run, as in the new-agent popup. servers is an optional list of aio server "
          "names for it to join."},
         {"inputSchema", schema({{"name", {{"type", "string"}, {"description", "New agent name"}}},
                                  {"directory", {{"type", "string"}, {"description", "Existing directory to run in"}}},
                                  {"command", {{"type", "string"}, {"description", "Command to run, e.g. claude"}}},
                                  {"servers",
                                   {{"type", "array"},
                                    {"items", {{"type", "string"}}},
                                    {"description", "aio server names to join. Omit to join none."}}}},
                                 json::array({"name", "directory", "command"}))}});
    return tools;
}

json call_tool(const McpContext& ctx, const std::string& name, const json& args) {
    Mailbox mailbox(ctx.mail_root, ctx.limits);

    if (name == "list_agents") {
        auto running = list_agents();
        auto role = [&](const std::string& who) {
            if (who == ctx.agent && ctx.manager) return std::string(" (manager)");
            for (const auto& a : running)
                if (a.name == who && a.manager) return std::string(" (manager)");
            return std::string();
        };
        std::string out = "You are " + ctx.agent + role(ctx.agent) + ".\n";
        std::string others;
        for (const auto& m : ctx.members)
            if (m != ctx.agent) others += "- " + m + role(m) + "\n";
        out += others.empty() ? "No other agents are on this server.\n" : "Other agents:\n" + others;
        return tool_text(out);
    }

    if (name == "stop_agent" || name == "kill_agent" || name == "create_agent") {
        if (!ctx.manager) return tool_text("only managers can control other agents", true);
        if (name == "create_agent") {
            if (!args.contains("name") || !args["name"].is_string() || !args.contains("directory") ||
                !args["directory"].is_string() || !args.contains("command") || !args["command"].is_string())
                return tool_text("arguments 'name', 'directory' and 'command' are required strings", true);
            auto agent_name = args["name"].get<std::string>();
            auto directory = args["directory"].get<std::string>();
            if (!directory.empty() && directory[0] == '~')
                directory = Platform::home_dir() + directory.substr(1);
            directory = Platform::absolute_path(directory);
            if (!Platform::is_directory(directory)) return tool_text("not a directory: " + directory, true);
            std::vector<std::string> servers;
            if (args.contains("servers") && args["servers"].is_array())
                for (const auto& s : args["servers"])
                    if (s.is_string()) servers.push_back(s.get<std::string>());
            auto err = create_agent(agent_name, directory, args["command"].get<std::string>(), servers, false, false);
            if (!err.empty()) return tool_text(err, true);
            log_event("manager '" + ctx.agent + "' created '" + agent_name + "'");
            return tool_text("Created worker '" + agent_name + "'. It is not a manager.");
        }
        if (!args.contains("name") || !args["name"].is_string())
            return tool_text("argument 'name' is required", true);
        auto target = args["name"].get<std::string>();
        auto err = name == "stop_agent" ? manager_stop(ctx.agent, target) : manager_kill(ctx.agent, target);
        if (!err.empty()) return tool_text(err, true);
        return tool_text(name == "stop_agent" ? "Stopped " + target + ". The agent is still running."
                                              : "Killed " + target + ".");
    }

    if (name == "send_message") {
        if (!args.contains("to") || !args.contains("message") || !args["message"].is_string())
            return tool_text("arguments 'to' and 'message' are required", true);
        std::vector<std::string> targets;
        if (args["to"].is_string()) targets.push_back(args["to"].get<std::string>());
        else if (args["to"].is_array())
            for (const auto& t : args["to"])
                if (t.is_string()) targets.push_back(t.get<std::string>());
        std::vector<std::string> unique;
        for (const auto& t : targets)
            if (std::find(unique.begin(), unique.end(), t) == unique.end()) unique.push_back(t);
        if (unique.empty()) return tool_text("'to' must be an agent name or a list of names", true);

        auto body = args["message"].get<std::string>();
        std::vector<std::string> deliverable;
        std::string errors;
        for (const auto& to : unique) {
            if (to == ctx.agent) {
                errors += (errors.empty() ? "" : "\n") + to + ": cannot send a message to yourself";
            } else if (std::find(ctx.members.begin(), ctx.members.end(), to) == ctx.members.end()) {
                errors += (errors.empty() ? "" : "\n") + to + ": no agent named '" + to + "' (use list_agents)";
            } else {
                deliverable.push_back(to);
            }
        }
        std::vector<std::string> sent;
        bool counted = false;
        for (const auto& to : deliverable) {
            auto result = mailbox.send(ctx.agent, to, body, ctx.members, deliverable, !counted);
            std::cout << timestamp() << "  " << ctx.agent << " -> " << to << "  "
                      << (result.ok() ? std::to_string(body.size()) + " chars" : "REFUSED: " + result.error)
                      << std::endl;
            if (result.ok()) {
                sent.push_back(to);
                counted = true;
            } else {
                errors += (errors.empty() ? "" : "\n") + to + ": " + result.error;
            }
        }
        if (sent.empty()) return tool_text("Not sent.\n" + errors, true);
        std::string who;
        for (const auto& s : sent) who += (who.empty() ? "" : ", ") + s;
        std::string out = "Message sent to " + who +
                          ". Each of them sees you (" + ctx.agent + ") as the sender";
        if (sent.size() > 1) out += " and sees the others in the recipient list";
        out += ".";
        if (!errors.empty()) out += "\nNot sent:\n" + errors;
        return tool_text(out);
    }

    if (name == "read_messages") {
        if (!ctx.limits.enabled) return tool_text("messaging is disabled in aio settings", true);
        auto messages = mailbox.read(ctx.agent);
        if (!messages.empty())
            std::cout << timestamp() << "  " << ctx.agent << " read " << messages.size()
                      << " message(s)" << std::endl;
        if (messages.empty()) return tool_text("No new messages for " + ctx.agent + ".");
        std::string out = "Messages for you (" + ctx.agent + "). Nobody else's mail is included.\n\n";
        for (const auto& m : messages) out += format_message(ctx.agent, m) + "\n---\n";
        return tool_text(out);
    }

    return tool_text("unknown tool: " + name, true);
}

}  // namespace

json handle_mcp_request(const McpContext& ctx, const json& request) {
    if (!request.is_object() || !request.contains("method") || !request.contains("id"))
        return nullptr;  // notification or response

    const auto method = request.value("method", "");
    json response = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
    const json params = request.value("params", json::object());

    if (method == "initialize") {
        response["result"] = {
            {"protocolVersion", params.value("protocolVersion", kProtocolVersion)},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "aio-" + ctx.server}, {"version", AIO_VERSION}}},
            {"instructions", messaging_guide(ctx) + manager_guide(ctx)},
        };
    } else if (method == "ping") {
        response["result"] = json::object();
    } else if (method == "tools/list") {
        response["result"] = {{"tools", tool_definitions(ctx)}};
    } else if (method == "tools/call") {
        response["result"] = call_tool(ctx, params.value("name", ""), params.value("arguments", json::object()));
    } else {
        response["error"] = {{"code", -32601}, {"message", "method not found: " + method}};
    }
    return response;
}

HttpReply handle_mcp_http(const ServerRegistry& registry, const std::string& server,
                          const MessagingConfig& limits, const std::string& authorization,
                          const std::string& body) {
    auto info = registry.get(server);
    if (!info) return {500, R"({"error":"server config missing"})"};

    const std::string prefix = "Bearer ";
    std::string agent;
    if (authorization.rfind(prefix, 0) == 0) {
        auto token = authorization.substr(prefix.size());
        for (const auto& [name, t] : info->members)
            if (!token.empty() && t == token) agent = name;
    }
    if (agent.empty()) return {401, R"({"error":"unknown or missing bearer token"})"};

    McpContext ctx{server, agent, registry.mail_root(server), limits, {}, false};
    for (const auto& [name, _] : info->members) ctx.members.push_back(name);
    for (const auto& a : list_agents())
        if (a.name == agent) ctx.manager = a.manager;

    json request;
    try {
        request = json::parse(body);
    } catch (const json::exception& e) {
        json err = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", e.what()}}}};
        return {400, err.dump()};
    }

    if (request.is_array()) {
        json responses = json::array();
        for (const auto& r : request)
            if (auto resp = handle_mcp_request(ctx, r); !resp.is_null()) responses.push_back(resp);
        if (responses.empty()) return {202, ""};
        return {200, responses.dump()};
    }
    auto response = handle_mcp_request(ctx, request);
    if (response.is_null()) return {202, ""};
    return {200, response.dump()};
}

int run_server(const std::string& server) {
    ServerRegistry registry;
    auto info = registry.get(server);
    if (!info) {
        std::cerr << "aio serve: no server named '" << server << "'\n";
        return 1;
    }

    httplib::Server http;
    std::mutex mutex;  // mailbox operations are not safe to interleave

    http.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard lock(mutex);
        auto reply = handle_mcp_http(registry, server, load_config().messaging,
                                     req.get_header_value("Authorization"), req.body);
        res.status = reply.status;
        if (!reply.body.empty()) res.set_content(reply.body, reply.content_type);
    });
    http.Get("/mcp", [](const httplib::Request&, httplib::Response& res) {
        res.status = 405;  // no server-initiated stream
    });
    http.Delete("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 405; });
    http.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });

    if (!http.bind_to_port("127.0.0.1", info->port)) {
        auto err = "port " + std::to_string(info->port) + " is unavailable";
        registry.set_last_error(server, err);
        std::cerr << "aio serve: " << err << "\n";
        return 1;
    }
    registry.set_last_error(server, "");
    std::cout << "aio server '" << server << "' listening on " << info->url() << std::endl;
    http.listen_after_bind();
    return 0;
}

}  // namespace aio
