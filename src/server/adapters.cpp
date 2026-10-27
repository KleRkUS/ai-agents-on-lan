#include "adapters.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "platform.hpp"

namespace aio {

using json = nlohmann::json;

namespace {

using Vars = std::map<std::string, std::string>;

constexpr const char* kEachServer = "{each_server}";

const char* kClaude = R"({
  "_comment": "Claude Code: servers go in a generated --mcp-config file; their tools are pre-approved.",
  "match": ["claude"],
  "config_file": {
    "content": {
      "mcpServers": {
        "{each_server}": {
          "type": "http",
          "url": "{url}",
          "headers": { "Authorization": "Bearer {token}" }
        }
      }
    }
  },
  "args": ["--mcp-config", "{config_file}", "--allowedTools", "{tools_csv}"]
}
)";

const char* kCodex = R"({
  "_comment": "OpenAI Codex CLI: -c overrides right after `codex`; tokens travel via environment variables.",
  "match": ["codex"],
  "insert_args": "after_command",
  "args_per_server": [
    "-c", "mcp_servers.{mcp_name}.url=\"{url}\"",
    "-c", "mcp_servers.{mcp_name}.bearer_token_env_var=\"{token_env}\""
  ],
  "env_per_server": { "{token_env}": "{token}" }
}
)";

const char* kCursor = R"({
  "_comment": "Cursor CLI reads MCP servers only from project config, so this merges them into <agent dir>/.cursor/mcp.json. Agents sharing a directory share that file. Delete this adapter to opt out.",
  "match": ["agent", "cursor-agent"],
  "project_file": {
    "path": ".cursor/mcp.json",
    "content": {
      "mcpServers": {
        "{each_server}": {
          "url": "{url}",
          "headers": { "Authorization": "Bearer {token}" }
        }
      }
    }
  },
  "args": ["--approve-mcps"]
}
)";

std::string substitute(std::string s, const Vars& vars) {
    for (const auto& [key, value] : vars) {
        auto needle = "{" + key + "}";
        for (auto pos = s.find(needle); pos != std::string::npos; pos = s.find(needle, pos + value.size()))
            s.replace(pos, needle.size(), value);
    }
    return s;
}

std::string pointer_token(const std::string& key) {
    std::string out;
    for (char c : key) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return out;
}

// `units` receives the JSON pointer of every per-server entry ("{each_server}" expansion).
json expand(const json& t, const Vars& vars, const std::vector<Vars>& per_server,
            std::vector<std::string>* units = nullptr, const std::string& at = "") {
    if (t.is_string()) return substitute(t.get<std::string>(), vars);
    if (t.is_array()) {
        json out = json::array();
        for (const auto& item : t) out.push_back(expand(item, vars, per_server));
        return out;
    }
    if (!t.is_object()) return t;
    json out = json::object();
    for (const auto& [key, value] : t.items()) {
        if (key == kEachServer) {
            for (const auto& server_vars : per_server) {
                const auto& name = server_vars.at("mcp_name");
                out[name] = expand(value, server_vars, per_server);
                if (units) units->push_back(at + "/" + pointer_token(name));
            }
        } else {
            auto k = substitute(key, vars);
            out[k] = expand(value, vars, per_server, units, at + "/" + pointer_token(k));
        }
    }
    return out;
}

std::string trace_path(const std::string& state_dir, const std::string& agent) {
    return Platform::join_path(state_dir, agent + ".traces.json");
}

constexpr const char* kTraceSuffix = ".traces.json";

bool dir_empty(const std::string& dir) {
    return Platform::list_files(dir).empty() && Platform::list_directories(dir).empty();
}

void merge_into(json& target, const json& patch) {
    if (!target.is_object() || !patch.is_object()) {
        target = patch;
        return;
    }
    for (const auto& [key, value] : patch.items()) merge_into(target[key], value);
}

bool plain_arg(const std::string& arg) {
    if (arg.empty()) return false;
    for (char c : arg)
        if (!std::isalnum(static_cast<unsigned char>(c)) && std::string("_-./,=:@+%").find(c) == std::string::npos)
            return false;
    return true;
}

std::string quote_arg(const std::string& arg) { return plain_arg(arg) ? arg : Platform::shell_quote(arg); }

bool string_array(const json& j) {
    if (!j.is_array()) return false;
    for (const auto& item : j)
        if (!item.is_string()) return false;
    return true;
}

bool string_map(const json& j) {
    if (!j.is_object()) return false;
    for (const auto& [key, value] : j.items())
        if (!value.is_string()) return false;
    return true;
}

bool write_atomic(const std::string& path, const std::string& contents) {
    auto tmp = path + ".tmp";
    return Platform::write_file(tmp, contents) && Platform::rename_file(tmp, path);
}

}  // namespace

std::string adapters_dir() { return Platform::join_path(Platform::config_dir(), "adapters"); }

std::vector<std::pair<std::string, std::string>> builtin_adapters() {
    return {{"claude", kClaude}, {"codex", kCodex}, {"cursor", kCursor}};
}

std::string validate_adapter(const json& spec) {
    if (!spec.is_object()) return "must be a JSON object";
    static const std::set<std::string> known = {"_comment", "match", "config_file", "project_file", "args",
                                                "args_per_server", "env", "env_per_server", "insert_args"};
    for (const auto& [key, value] : spec.items())
        if (!known.count(key)) return "unknown key '" + key + "'";
    if (!spec.contains("match") || !string_array(spec["match"]) || spec["match"].empty())
        return "'match' must be a non-empty list of command names";
    for (const char* key : {"args", "args_per_server"})
        if (spec.contains(key) && !string_array(spec[key])) return std::string("'") + key + "' must be a list of strings";
    for (const char* key : {"env", "env_per_server"})
        if (spec.contains(key) && !string_map(spec[key]))
            return std::string("'") + key + "' must map names to strings";
    if (spec.contains("insert_args")) {
        auto& v = spec["insert_args"];
        if (!v.is_string() || (v != "end" && v != "after_command"))
            return "'insert_args' must be \"end\" or \"after_command\"";
    }
    if (spec.contains("config_file")) {
        auto& f = spec["config_file"];
        if (!f.is_object() || !f.contains("content")) return "'config_file' needs a 'content' object";
        if (f.contains("name") && !f["name"].is_string()) return "'config_file.name' must be a string";
    }
    if (spec.contains("project_file")) {
        auto& f = spec["project_file"];
        if (!f.is_object() || !f.contains("content") || !f["content"].is_object() || !f.contains("path") ||
            !f["path"].is_string())
            return "'project_file' needs a 'path' string and a 'content' object";
        auto path = f["path"].get<std::string>();
        if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos ||
            path.find("..") != std::string::npos)
            return "'project_file.path' must be relative to the agent directory";
    }
    return {};
}

AdapterSet load_adapters(const std::string& dir) {
    AdapterSet set{dir, {}, {}};
    if (!Platform::is_directory(dir)) {
        Platform::create_directories(dir);
        for (const auto& [name, contents] : builtin_adapters())
            Platform::write_file(Platform::join_path(dir, name + ".json"), contents);
    }
    for (const auto& file : Platform::list_files(dir)) {
        if (file.size() <= 5 || file.substr(file.size() - 5) != ".json") continue;
        auto contents = Platform::read_file(Platform::join_path(dir, file));
        json spec = json::parse(contents.value_or(""), nullptr, false);
        if (spec.is_discarded()) {
            set.errors.push_back(file + ": invalid JSON");
            continue;
        }
        if (auto err = validate_adapter(spec); !err.empty()) {
            set.errors.push_back(file + ": " + err);
            continue;
        }
        set.adapters.push_back({file.substr(0, file.size() - 5), spec["match"].get<std::vector<std::string>>(), spec});
    }
    return set;
}

std::string command_word(const std::string& command) {
    auto start = command.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    auto word = command.substr(start, command.find_first_of(" \t", start) - start);
    auto slash = word.find_last_of("/\\");
    return slash == std::string::npos ? word : word.substr(slash + 1);
}

const Adapter* find_adapter(const AdapterSet& set, const std::string& command) {
    auto word = command_word(command);
    for (const auto& a : set.adapters)
        for (const auto& m : a.match)
            if (m == word) return &a;
    return nullptr;
}

LaunchWiring wire_messaging(const AdapterSet& adapters, const std::string& agent,
                            const std::string& command, const std::string& work_dir,
                            const std::vector<ServerMembership>& servers, const std::string& state_dir) {
    LaunchWiring wiring{command, {}, false, {}};
    if (servers.empty()) return wiring;
    const Adapter* adapter = find_adapter(adapters, command);
    if (!adapter) {
        wiring.error = "no adapter for '" + command_word(command) + "' in " + adapters.dir;
        return wiring;
    }
    const json& spec = adapter->spec;

    std::vector<Vars> per_server;
    std::string tools;
    for (const auto& m : servers) {
        per_server.push_back({{"server", m.server.name},
                              {"mcp_name", m.server.mcp_name()},
                              {"url", m.server.url()},
                              {"port", std::to_string(m.server.port)},
                              {"token", m.token},
                              {"token_env", m.server.token_env()}});
        tools += (tools.empty() ? "mcp__" : ",mcp__") + m.server.mcp_name();
    }
    Vars vars{{"agent", agent}, {"tools_csv", tools}};

    json trace = {{"files", json::array()}, {"project", json::array()}};
    auto save_trace = [&] {
        Platform::create_directories(state_dir);
        write_atomic(trace_path(state_dir, agent), trace.dump(2));
    };

    if (spec.contains("config_file")) {
        auto name = substitute(spec["config_file"].value("name", "{agent}.mcp.json"), vars);
        auto file = Platform::join_path(state_dir, name);
        vars["config_file"] = file;
        if (!Platform::create_directories(state_dir) ||
            !write_atomic(file, expand(spec["config_file"]["content"], vars, per_server).dump(2))) {
            wiring.error = "could not write " + file;
            return wiring;
        }
        trace["files"].push_back(file);
        save_trace();
    }
    for (auto& server_vars : per_server)
        for (const auto& [key, value] : vars) server_vars.emplace(key, value);

    if (spec.contains("project_file")) {
        auto file = Platform::join_path(work_dir, spec["project_file"]["path"].get<std::string>());
        auto previous = Platform::read_file(file);
        json existing = json::parse(previous.value_or("{}"), nullptr, false);
        if (existing.is_discarded() || !existing.is_object()) {
            wiring.error = file + " exists but is not a JSON object; not touching it";
            return wiring;
        }
        std::vector<std::string> units;
        auto patch = expand(spec["project_file"]["content"], vars, per_server, &units);

        // Containers (e.g. /mcpServers) that only exist because of us, deepest first.
        std::vector<std::string> containers;
        for (const auto& unit : units)
            for (auto cut = unit.rfind('/'); cut != 0 && cut != std::string::npos; cut = unit.rfind('/', cut - 1)) {
                auto prefix = unit.substr(0, cut);
                if (!existing.contains(json::json_pointer(prefix)) &&
                    std::find(containers.begin(), containers.end(), prefix) == containers.end())
                    containers.push_back(prefix);
            }
        std::sort(containers.begin(), containers.end(),
                  [](const auto& a, const auto& b) { return a.size() > b.size(); });

        std::vector<std::string> created_dirs;
        for (auto dir = Platform::parent_path(file); dir.size() > work_dir.size() && !Platform::is_directory(dir);
             dir = Platform::parent_path(dir))
            created_dirs.push_back(dir);

        merge_into(existing, patch);
        if (!Platform::create_directories(Platform::parent_path(file)) || !write_atomic(file, existing.dump(2))) {
            wiring.error = "could not write " + file;
            return wiring;
        }
        trace["project"].push_back({{"file", file},
                                    {"units", units},
                                    {"containers", containers},
                                    {"created_file", !previous.has_value()},
                                    {"created_dirs", created_dirs}});
        save_trace();
    }

    const json args = spec.value("args", json::array());
    const json args_per_server = spec.value("args_per_server", json::array());
    const json env = spec.value("env", json::object());
    const json env_per_server = spec.value("env_per_server", json::object());

    std::string extra;
    for (const auto& arg : args) extra += " " + quote_arg(substitute(arg, vars));
    for (const auto& server_vars : per_server)
        for (const auto& arg : args_per_server) extra += " " + quote_arg(substitute(arg, server_vars));

    for (const auto& [key, value] : env.items())
        wiring.env.emplace_back(substitute(key, vars), substitute(value.get<std::string>(), vars));
    for (const auto& server_vars : per_server)
        for (const auto& [key, value] : env_per_server.items())
            wiring.env.emplace_back(substitute(key, server_vars), substitute(value.get<std::string>(), server_vars));

    if (spec.value("insert_args", "end") == "after_command") {
        auto start = command.find_first_not_of(" \t");
        auto end = start == std::string::npos ? std::string::npos : command.find_first_of(" \t", start);
        wiring.command = end == std::string::npos ? command + extra : command.substr(0, end) + extra + command.substr(end);
    } else {
        wiring.command = command + extra;
    }
    wiring.wired = true;
    return wiring;
}

std::string agent_state_dir() { return Platform::join_path(Platform::state_dir(), "agents"); }

std::vector<std::string> traced_agents(const std::string& state_dir) {
    std::vector<std::string> agents;
    const std::string suffix = kTraceSuffix;
    for (const auto& file : Platform::list_files(state_dir))
        if (file.size() > suffix.size() && file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0)
            agents.push_back(file.substr(0, file.size() - suffix.size()));
    return agents;
}

void remove_agent_traces(const std::string& agent, const std::string& state_dir) {
    Platform::remove_file(Platform::join_path(state_dir, agent + ".briefed"));
    auto path = trace_path(state_dir, agent);
    json trace = json::parse(Platform::read_file(path).value_or(""), nullptr, false);
    if (!trace.is_object()) {
        Platform::remove_file(path);
        return;
    }

    std::vector<json> others;
    for (const auto& other : traced_agents(state_dir)) {
        if (other == agent) continue;
        json t = json::parse(Platform::read_file(trace_path(state_dir, other)).value_or(""), nullptr, false);
        if (t.is_object()) others.push_back(std::move(t));
    }
    auto others_entries = [&](const std::string& file) {
        std::vector<json> entries;
        for (const auto& t : others)
            for (const auto& p : t.value("project", json::array()))
                if (p.value("file", "") == file) entries.push_back(p);
        return entries;
    };
    auto held = [](const std::vector<json>& entries, const std::string& pointer) {
        for (const auto& p : entries)
            for (const char* key : {"units", "containers"})
                for (const auto& v : p.value(key, json::array()))
                    if (v == pointer) return true;
        return false;
    };

    for (const auto& file : trace.value("files", json::array()))
        if (file.is_string()) Platform::remove_file(file.get<std::string>());

    for (const auto& p : trace.value("project", json::array())) {
        auto file = p.value("file", "");
        auto contents = Platform::read_file(file);
        if (file.empty() || !contents) continue;
        json doc = json::parse(*contents, nullptr, false);
        if (!doc.is_object()) continue;  // user replaced it; leave alone
        auto entries = others_entries(file);

        auto erase = [&](const std::string& pointer, bool only_if_empty) {
            json::json_pointer ptr(pointer);
            if (held(entries, pointer) || !doc.contains(ptr)) return;
            if (only_if_empty && !(doc.at(ptr).is_object() && doc.at(ptr).empty())) return;
            doc[ptr.parent_pointer()].erase(ptr.back());
        };
        try {
            for (const auto& u : p.value("units", json::array())) erase(u.get<std::string>(), false);
            for (const auto& c : p.value("containers", json::array())) erase(c.get<std::string>(), true);
        } catch (const json::exception&) {
            continue;
        }

        if (p.value("created_file", false) && entries.empty() && doc.empty()) {
            Platform::remove_file(file);
            for (const auto& d : p.value("created_dirs", json::array()))
                if (d.is_string() && Platform::is_directory(d.get<std::string>()) && dir_empty(d.get<std::string>()))
                    Platform::remove_all(d.get<std::string>());
        } else {
            write_atomic(file, doc.dump(2));
        }
    }
    Platform::remove_file(path);
}

}  // namespace aio
