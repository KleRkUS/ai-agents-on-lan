#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <optional>
#include <thread>
#include <tuple>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "api.hpp"
#include "platform.hpp"
#include "tmux.hpp"
#include "ui.hpp"

namespace aio {

using namespace ftxui;

namespace {

constexpr int kListHeight = 6;

std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    return Platform::home_dir() + path.substr(1);
}

std::string shorten_home(const std::string& path) {
    auto home = Platform::home_dir();
    if (!home.empty() && path.rfind(home, 0) == 0) return "~" + path.substr(home.size());
    return path;
}

std::optional<int> parse_int(const std::string& s) {
    try {
        size_t used = 0;
        int v = std::stoi(s, &used);
        if (used == s.size()) return v;
    } catch (...) {
    }
    return std::nullopt;
}

// Parses "#rrggbb"; anything else falls back to the terminal's default color.
Color hex_color(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return Color::Default;
    try {
        auto v = std::stoul(hex.substr(1), nullptr, 16);
        return Color::RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    } catch (...) {
        return Color::Default;
    }
}

std::string slug(const std::string& s) {
    std::string out;
    for (char c : s) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    return out;
}

// A text input with a fixed cursor slot so the cursor can be moved to the end on open.
struct Field {
    std::string value;
    int cursor = 0;
    Component input;

    void make(const char* placeholder, std::function<void()> on_enter) {
        InputOption opt;
        opt.content = &value;
        opt.placeholder = placeholder;
        opt.multiline = false;
        opt.on_enter = std::move(on_enter);
        opt.cursor_position = &cursor;
        input = Input(opt);
    }
    void set(std::string v) {
        value = std::move(v);
        cursor = static_cast<int>(value.size());
    }
    Element render(const char* label) const {
        return vbox({text(label) | dim, input->Render() | bgcolor(Color::GrayDark)});
    }
};

// Lets Tab reach the parent container instead of cycling through menu entries.
Component PassTab(Component child) {
    class Impl : public ComponentBase {
    public:
        explicit Impl(Component c) { Add(std::move(c)); }
        bool OnEvent(Event e) override {
            if (e == Event::Tab || e == Event::TabReverse) return false;
            return ComponentBase::OnEvent(e);
        }
    };
    return Make<Impl>(std::move(child));
}

std::vector<std::string> preset_labels(const std::vector<Preset>& presets) {
    std::vector<std::string> labels;
    for (const auto& p : presets) labels.push_back(p.name + "  ·  " + p.command);
    return labels;
}

Element scroll_list(Component& menu, bool empty, const char* empty_text) {
    Element body = empty ? text(empty_text) | dim : menu->Render() | vscroll_indicator | frame;
    return body | size(HEIGHT, EQUAL, kListHeight) | border;
}

}  // namespace

int run_sidebar() {
    auto screen = ScreenInteractive::Fullscreen();
    Control control;
    control.zoom(false);

    Config config;
    AdapterSet adapters;
    std::vector<Agent> agents;
    std::string viewport;
    std::vector<ControlState::ServerRow> servers;
    std::map<std::string, size_t> unread_counts;
    std::map<std::string, size_t> server_unread;
    int next_port = 47800;
    std::string host_name;
    std::string host_id;
    std::string control_error;
    int selected = 0;
    bool show_form = false;
    bool show_settings = false;
    bool show_server_form = false;
    bool show_quit = false;
    std::vector<JoinedHost> machines;
    int selected_machine = 0;
    bool choosing_machine = false;

    enum class Tab { Agents, Servers };
    Tab tab = Tab::Agents;

    int selected_server = 0;

    auto clamp_selection = [](int& sel, size_t count) {
        sel = std::clamp(sel, 0, std::max(0, static_cast<int>(count) - 1));
    };

    auto machine_listed = [&](const std::string& id) {
        for (const auto& machine : machines)
            if (machine.id == id) return true;
        return false;
    };
    auto apply_target = [&](const std::string& id) {
        if (control_endpoint().target == id) return;
        if (id.empty()) Platform::unset_env("AIO_TARGET");
        else Platform::set_env("AIO_TARGET", id);
        if (id.empty()) tmux({"set-environment", "-u", "-t", std::string(kUiSession), "AIO_TARGET"});
        else tmux({"set-environment", "-t", std::string(kUiSession), "AIO_TARGET", id});
    };
    auto sync_machines = [&] {
        if (!control_endpoint().remote) return;
        machines = control.hosts();
        clamp_selection(selected_machine, machines.size());
        auto current = control_endpoint().target;
        if (machine_listed(current)) return;
        if (machines.size() == 1) {
            choosing_machine = false;
            apply_target(machines[0].id);
        } else if (machines.size() > 1 && machine_listed(host_id)) {
            apply_target(host_id);
        } else if (machines.size() > 1) {
            apply_target("");
            choosing_machine = true;
            control_error.clear();
        } else {
            apply_target("");
            choosing_machine = false;
        }
    };
    auto load_state = [&](ControlState s) {
        if (!s.error.empty()) {
            control_error = std::move(s.error);
            return;
        }
        control_error.clear();
        agents = std::move(s.agents);
        servers = std::move(s.servers);
        viewport = std::move(s.viewport);
        config = std::move(s.config);
        adapters = std::move(s.adapters);
        next_port = s.next_port;
        host_name = std::move(s.host_name);
        host_id = std::move(s.host_id);
        unread_counts = std::move(s.unread_counts);
        server_unread.clear();
        for (const auto& row : servers) server_unread[row.info.name] = row.unread;
        clamp_selection(selected, agents.size());
        clamp_selection(selected_server, servers.size());
    };
    auto refresh = [&] {
        sync_machines();
        if (choosing_machine) return;
        load_state(control.state());
    };
    sync_machines();
    if (!choosing_machine) load_state(control.state());

    auto open_popup = [&](bool& flag) {
        flag = true;
        control.zoom(true);
    };
    auto close_popups = [&] {
        show_form = show_settings = show_server_form = show_quit = false;
        control.zoom(false);
    };

    // ---------------- new agent popup ----------------
    Field name, dir, command;
    std::string form_error;
    bool name_is_auto = true;
    std::vector<std::string> form_presets;
    int form_preset = 0;
    std::vector<std::string> form_server_names;
    std::deque<bool> form_server_checked;  // deque: Checkbox keeps pointers to elements
    bool form_manager = false;
    auto form_servers = Container::Vertical({});
    auto manager_box = Checkbox("Manager — can stop, kill and create other agents", &form_manager);

    auto submit = [&] {
        auto path = Platform::absolute_path(expand_home(dir.value));
        if (auto err = validate_agent_name(name.value); !err.empty()) return void(form_error = err);
        if (!Platform::is_directory(path)) return void(form_error = "not a directory");
        if (command.value.empty()) return void(form_error = "command is required");
        std::vector<std::string> chosen;
        for (size_t i = 0; i < form_server_names.size(); ++i)
            if (form_server_checked[i]) chosen.push_back(form_server_names[i]);
        if (!chosen.empty() && !config.messaging.enabled)
            return void(form_error = "messaging is disabled in settings (s)");
        if (auto err = control.create_agent(name.value, path, command.value, chosen, form_manager); !err.empty())
            return void(form_error = err);
        close_popups();
        refresh();
        selected = static_cast<int>(agents.size()) - 1;
    };

    auto apply_preset = [&] {
        if (form_preset < 0 || form_preset >= static_cast<int>(config.presets.size())) return;
        const auto& p = config.presets[form_preset];
        command.set(p.command);
        if (name_is_auto) name.set(slug(p.name) + "-" + std::to_string(agents.size() + 1));
    };

    MenuOption form_menu_opt = MenuOption::Vertical();
    form_menu_opt.entries = &form_presets;
    form_menu_opt.selected = &form_preset;
    form_menu_opt.on_change = apply_preset;
    form_menu_opt.on_enter = submit;
    auto form_menu = PassTab(Menu(form_menu_opt));

    name.make("agent name", submit);
    dir.make("~/project", submit);
    command.make("command to run", submit);
    name.input |= CatchEvent([&](Event e) {
        if (e.is_character() || e == Event::Backspace || e == Event::Delete) name_is_auto = false;
        return false;
    });

    auto form_container =
        Container::Vertical({form_menu, name.input, dir.input, command.input,
                             CatchEvent(manager_box, [&](Event e) {
                                 if (e != Event::Return) return false;
                                 submit();
                                 return true;
                             }),
                             PassTab(form_servers) | CatchEvent([&](Event e) {
                                 if (e != Event::Return) return false;
                                 submit();
                                 return true;
                             })});
    auto form_view = Renderer(form_container, [&] {
        Elements rows = {
            text(" New agent ") | bold | center,
            separator(),
            text("Preset  (↑/↓ pick, tab next field)") | dim,
            scroll_list(form_menu, form_presets.empty(), "no presets — add some in settings (s)"),
            name.render("Name"),
            dir.render("Directory"),
            command.render("Command (edit freely)"),
            manager_box->Render(),
            text("MCP servers to join  (↑/↓ move, space toggle)") | dim,
        };
        if (form_server_names.empty()) {
            rows.push_back(text("no servers — create one on the MCP tab (2)") | dim | border);
        } else {
            rows.push_back(form_servers->Render() | vscroll_indicator | frame |
                           size(HEIGHT, LESS_THAN, 5) | border);
            bool any = std::find(form_server_checked.begin(), form_server_checked.end(), true) !=
                       form_server_checked.end();
            if (any && !find_adapter(adapters, command.value))
                rows.push_back(paragraph("no adapter for '" + command_word(command.value) +
                                         "' — it cannot join servers (see Settings › Adapters)") |
                               color(Color::Yellow));
        }
        if (!form_error.empty()) rows.push_back(text(form_error) | color(Color::Red));
        rows.push_back(separator());
        rows.push_back(text("enter create   tab next   esc cancel") | dim | center);
        return vbox(rows) | size(WIDTH, EQUAL, 60) | borderRounded;
    });
    form_view |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            close_popups();
            return true;
        }
        return false;
    });

    auto open_form = [&] {
        form_error.clear();
        refresh();
        form_presets = preset_labels(config.presets);
        form_preset = 0;
        name_is_auto = true;
        name.set("agent-" + std::to_string(agents.size() + 1));
        dir.set(shorten_home(Platform::current_dir()));
        command.set(config.default_command);
        form_manager = false;
        form_servers->DetachAllChildren();
        form_server_names.clear();
        form_server_checked.clear();
        for (const auto& s : servers) {
            form_server_names.push_back(s.info.name);
            form_server_checked.push_back(false);
            auto label = s.info.name + (s.running ? "" : "  (stopped)");
            form_servers->Add(Checkbox(label, &form_server_checked.back()));
        }
        open_popup(show_form);
        if (form_presets.empty()) name.input->TakeFocus();
        else form_menu->TakeFocus();
    };

    // ---------------- settings popup ----------------
    Config draft;
    Field default_cmd, key_detach, key_kill, preset_name, preset_cmd;
    Field max_chars, per_minute, max_hops, max_inbox, idle_secs;
    std::string settings_error;
    std::vector<std::string> settings_presets;
    int settings_preset = 0;

    auto sync_preset_labels = [&] {
        settings_presets = preset_labels(draft.presets);
        int n = static_cast<int>(settings_presets.size());
        settings_preset = std::clamp(settings_preset, 0, std::max(0, n - 1));
    };

    int editing_preset = -1;  // index being edited, -1 when the fields add a new preset
    std::string preset_btn_label = "Add preset";

    auto stop_editing = [&] {
        editing_preset = -1;
        preset_btn_label = "Add preset";
        preset_name.set("");
        preset_cmd.set("");
    };
    auto edit_preset = [&] {
        if (draft.presets.empty()) return;
        editing_preset = settings_preset;
        preset_btn_label = "Update preset";
        preset_name.set(draft.presets[editing_preset].name);
        preset_cmd.set(draft.presets[editing_preset].command);
        settings_error.clear();
        preset_cmd.input->TakeFocus();
    };
    auto add_preset = [&] {
        Preset preset{preset_name.value, preset_cmd.value};
        if (auto err = validate_preset(preset); !err.empty()) return void(settings_error = err);
        if (editing_preset >= 0) {
            draft.presets[editing_preset] = preset;
            settings_preset = editing_preset;
        } else {
            draft.presets.push_back(preset);
            settings_preset = static_cast<int>(draft.presets.size()) - 1;
        }
        stop_editing();
        settings_error.clear();
        sync_preset_labels();
    };
    auto delete_preset = [&] {
        if (draft.presets.empty()) return;
        draft.presets.erase(draft.presets.begin() + settings_preset);
        stop_editing();
        sync_preset_labels();
    };
    auto save_settings = [&] {
        draft.default_command = default_cmd.value;
        draft.key_detach = key_detach.value;
        draft.key_kill = key_kill.value;
        auto& m = draft.messaging;
        for (auto [field, out, label] : {std::tuple{&max_chars, &m.max_message_chars, "max message chars"},
                                         {&per_minute, &m.max_messages_per_minute, "messages per minute"},
                                         {&max_hops, &m.max_hops, "max hops"},
                                         {&max_inbox, &m.max_inbox, "inbox limit"},
                                         {&idle_secs, &m.idle_seconds, "idle seconds"}}) {
            auto v = parse_int(field->value);
            if (!v) return void(settings_error = std::string(label) + " must be a number");
            *out = *v;
        }
        if (auto err = validate_keys(draft); !err.empty()) return void(settings_error = err);
        if (auto err = validate_messaging(m); !err.empty()) return void(settings_error = err);
        if (auto err = control.save_config(draft); !err.empty()) return void(settings_error = err);
        config = draft;
        close_popups();
    };

    default_cmd.make("claude", save_settings);
    key_detach.make("C-x", save_settings);
    key_kill.make("d", save_settings);
    preset_name.make("e.g. claude resume", add_preset);
    preset_cmd.make("e.g. claude --resume", add_preset);
    max_chars.make("4000", save_settings);
    per_minute.make("10", save_settings);
    max_hops.make("6", save_settings);
    max_inbox.make("50", save_settings);
    idle_secs.make("3", save_settings);

    auto enabled_box = Checkbox("Agents can message each other", &draft.messaging.enabled);
    auto deliver_box = Checkbox("Notify idle agents of new messages", &draft.messaging.auto_deliver);

    MenuOption settings_menu_opt = MenuOption::Vertical();
    settings_menu_opt.entries = &settings_presets;
    settings_menu_opt.selected = &settings_preset;
    auto settings_menu = PassTab(Menu(settings_menu_opt));
    settings_menu |= CatchEvent([&](Event e) {
        if (e == Event::Delete || e == Event::Character('x')) {
            delete_preset();
            return true;
        }
        if (e == Event::Return || e == Event::Character('e')) {
            edit_preset();
            return true;
        }
        return false;
    });

    auto ascii = ButtonOption::Ascii();
    auto delete_btn = Button("Delete selected", delete_preset, ascii);
    auto add_btn = Button(&preset_btn_label, add_preset, ascii);
    auto new_btn = Button("Cancel edit", stop_editing, ascii);
    auto save_btn = Button("Save", save_settings, ascii);
    auto cancel_btn = Button("Cancel", close_popups, ascii);

    auto settings_container = Container::Vertical({
        default_cmd.input, key_detach.input, key_kill.input, settings_menu, delete_btn,
        preset_name.input, preset_cmd.input, add_btn,
        Maybe(new_btn, [&] { return editing_preset >= 0; }),
        enabled_box, deliver_box, max_chars.input, per_minute.input, max_hops.input,
        max_inbox.input, idle_secs.input,
        save_btn, cancel_btn,
    });

    auto settings_view = Renderer(settings_container, [&] {
        Elements rows = {
            text(" Settings ") | bold | center,
            separator(),
            default_cmd.render("Default command"),
            hbox({
                key_detach.render("Back-to-list key (C-x, M-q, F12)") | flex,
                text("  "),
                key_kill.render("Kill key") | flex,
            }),
            text(""),
            text("Presets  (↑/↓ scroll, enter edit, x delete)") | dim,
            scroll_list(settings_menu, settings_presets.empty(), "no presets"),
            delete_btn->Render(),
            text(""),
            text(editing_preset >= 0 ? "Edit preset" : "New preset") | bold,
            preset_name.render("Name"),
            preset_cmd.render("Command"),
            editing_preset >= 0 ? hbox({add_btn->Render(), text("  "), new_btn->Render()}) : add_btn->Render(),
            text(""),
            text("Messaging") | bold,
            enabled_box->Render(),
            deliver_box->Render(),
            hbox({max_chars.render("Max message chars") | flex, text("  "),
                  per_minute.render("Messages / minute / agent") | flex}),
            hbox({max_hops.render("Max reply chain (hops)") | flex, text("  "),
                  max_inbox.render("Unread inbox limit") | flex}),
            idle_secs.render("Idle seconds before notifying"),
            text("Limits apply to all MCP servers (tab 2).") | dim,
            text(""),
        };
        if (control_endpoint().remote) {
            auto driven = host_name.empty() ? std::string("the selected machine") : host_name;
            rows.push_back(text("Controlling") | bold);
            rows.push_back(paragraph("Agents run on " + driven + (host_id.empty() ? "" : " (id " + host_id + ")") +
                                     ". This device draws them and sends keystrokes.") |
                           dim);
            if (machines.size() > 1)
                rows.push_back(paragraph("Press m in the list to switch machine.") | dim);
            else
                rows.push_back(paragraph("This is the only machine joined to the relay.") | dim);
        } else {
            rows.push_back(text("This machine") | bold);
            rows.push_back(paragraph("Agents run here (" + (host_name.empty() ? "this machine" : host_name) +
                                     (host_id.empty() ? "" : ", id " + host_id) +
                                     "). Other devices do not connect to this port.") |
                           dim);
            rows.push_back(paragraph("On a machine they can reach:  aio --relay") | dim);
            rows.push_back(paragraph("On this machine:  aio --join ADDRESS --token TOKEN") | dim);
            rows.push_back(paragraph("TOKEN is the token created on the relay. Guest Wi-Fi often blocks devices from "
                                     "seeing each other.") |
                           dim);
        }
        rows.push_back(text(""));
        rows.push_back(text("Adapters  (how each CLI connects to servers)") | bold);
        for (const auto& a : adapters.adapters) {
            std::string match;
            for (const auto& m : a.match) match += (match.empty() ? "" : ", ") + m;
            rows.push_back(hbox({text("  ✓ " + a.name) | color(Color::Green), text("  " + match) | dim}));
        }
        for (const auto& err : adapters.errors) rows.push_back(paragraph("  ✗ " + err) | color(Color::Red));
        if (adapters.adapters.empty() && adapters.errors.empty()) rows.push_back(text("  none") | dim);
        rows.push_back(paragraph("Edit or add *.json files in " + shorten_home(adapters.dir) +
                                 " — reloaded when this popup opens.") |
                       dim);
        int max_height = std::max(10, Terminal::Size().dimy - 8);
        Elements footer;
        if (!settings_error.empty()) footer.push_back(text(settings_error) | color(Color::Red));
        footer.push_back(separator());
        footer.push_back(hbox({save_btn->Render(), text("  "), cancel_btn->Render()}) | center);
        footer.push_back(text(config_path()) | dim | center);
        return vbox({
                   vbox(rows) | vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, max_height),
                   vbox(footer),
               }) |
               size(WIDTH, EQUAL, 64) | borderRounded;
    });
    settings_view |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            close_popups();
            return true;
        }
        return false;
    });

    auto open_settings = [&] {
        refresh();
        draft = config;
        settings_error.clear();
        settings_preset = 0;
        sync_preset_labels();
        default_cmd.set(draft.default_command);
        key_detach.set(draft.key_detach);
        key_kill.set(draft.key_kill);
        stop_editing();
        const auto& m = draft.messaging;
        max_chars.set(std::to_string(m.max_message_chars));
        per_minute.set(std::to_string(m.max_messages_per_minute));
        max_hops.set(std::to_string(m.max_hops));
        max_inbox.set(std::to_string(m.max_inbox));
        idle_secs.set(std::to_string(m.idle_seconds));
        open_popup(show_settings);
        default_cmd.input->TakeFocus();
    };

    // ---------------- new server popup ----------------
    Field srv_name, srv_port;
    std::string server_form_error;

    auto submit_server = [&] {
        auto port = parse_int(srv_port.value);
        if (!port) return void(server_form_error = "port must be a number");
        if (auto err = control.create_server(srv_name.value, *port); !err.empty())
            return void(server_form_error = err);
        close_popups();
        refresh();
        for (int i = 0; i < static_cast<int>(servers.size()); ++i)
            if (servers[i].info.name == srv_name.value) selected_server = i;
    };
    srv_name.make("e.g. backend-team", submit_server);
    srv_port.make("47800", submit_server);

    auto server_form_view = Renderer(Container::Vertical({srv_name.input, srv_port.input}), [&] {
        Elements rows = {
            text(" New MCP server ") | bold | center,
            separator(),
            srv_name.render("Name (a-z, 0-9, -)"),
            srv_port.render("Port (localhost only)"),
            text("Agents join servers when you create them (n on tab 1).") | dim,
        };
        if (!server_form_error.empty()) rows.push_back(text(server_form_error) | color(Color::Red));
        rows.push_back(separator());
        rows.push_back(text("enter create   tab next   esc cancel") | dim | center);
        return vbox(rows) | size(WIDTH, EQUAL, 60) | borderRounded;
    });
    server_form_view |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            close_popups();
            return true;
        }
        return false;
    });

    auto open_server_form = [&] {
        server_form_error.clear();
        srv_name.set("");
        refresh();
        srv_port.set(std::to_string(next_port));
        open_popup(show_server_form);
        srv_name.input->TakeFocus();
    };

    // ---------------- quit popup ----------------
    auto running_servers = [&] {
        return std::count_if(servers.begin(), servers.end(), [](const ControlState::ServerRow& s) { return s.running; });
    };
    auto quit = [&] {
        if (control_endpoint().remote) tmux({"kill-session", "-t", std::string(kUiSession)});
        else control.shutdown();
        screen.Exit();
    };
    auto quit_btn = Button("Quit", quit, ButtonOption::Ascii());
    auto stay_btn = Button("Cancel", close_popups, ButtonOption::Ascii());
    auto quit_view = Renderer(Container::Horizontal({quit_btn, stay_btn}), [&] {
        auto n_agents = agents.size();
        auto n_servers = running_servers();
        Elements rows = {text(" Quit aio? ") | bold | center, separator()};
        if (control_endpoint().remote) {
            auto driven = host_name.empty() ? std::string("the other machine") : host_name;
            rows.push_back(paragraph("This disconnects this device. Agents and servers on " + driven + " keep running."));
        } else if (n_agents || n_servers) {
            rows.push_back(text("This stops:"));
            if (n_agents) rows.push_back(text("  • " + std::to_string(n_agents) + " agent(s) — their sessions end"));
            if (n_servers) rows.push_back(text("  • " + std::to_string(n_servers) + " MCP server(s)"));
            rows.push_back(paragraph("Config files aio wrote for agents are removed. Server definitions and "
                                     "settings are kept.") |
                           dim);
        } else {
            rows.push_back(text("Nothing is running."));
        }
        rows.push_back(separator());
        rows.push_back(hbox({quit_btn->Render(), text("   "), stay_btn->Render()}) | center);
        rows.push_back(text("y quit   n / esc cancel") | dim | center);
        return vbox(rows) | size(WIDTH, EQUAL, 52) | borderRounded;
    });
    quit_view |= CatchEvent([&](Event e) {
        if (e == Event::Character('y')) return quit(), true;
        if (e == Event::Escape || e == Event::Character('n') || e == Event::Character('q')) {
            close_popups();
            return true;
        }
        return false;
    });

    // ---------------- tab views ----------------
    auto tab_bar = [&] {
        auto label = [&](const char* text_, Tab t) {
            auto e = text(text_);
            return tab == t ? e | bold | inverted : e | dim;
        };
        return hbox({label(" 1 Agents ", Tab::Agents), text(" "), label(" 2 MCP ", Tab::Servers)}) | center;
    };

    auto servers_body = [&] {
        Elements rows;
        for (int i = 0; i < static_cast<int>(servers.size()); ++i) {
            const auto& s = servers[i];
            auto status = s.running ? text(" ● ") | color(Color::Green) : text(" ○ ") | color(Color::Red);
            auto unread = server_unread.count(s.info.name) ? server_unread.at(s.info.name) : 0;
            Elements lines = {
                hbox({status, text(s.info.name) | bold}),
                hbox({text("   "), text(":" + std::to_string(s.info.port) + "  " +
                                         std::to_string(s.info.members.size()) + " agent(s)" +
                                         (unread ? "  ✉ " + std::to_string(unread) : "")) | dim}),
            };
            if (!s.running)
                lines.push_back(hbox({text("   "), text(s.error.empty() ? "stopped" : s.error) |
                                                        color(Color::Red) | dim}));
            if (i == selected_server) {
                for (const auto& [member, _] : s.info.members) {
                    bool manager = std::any_of(agents.begin(), agents.end(), [&](const Agent& a) {
                        return a.name == member && a.manager;
                    });
                    lines.push_back(hbox({text("   · "), text(member) | color(Color::Cyan),
                                          manager ? text(" manager") | color(Color::Magenta) : text("")}));
                }
            }
            auto entry = vbox(lines);
            if (i == selected_server) entry = entry | inverted;
            rows.push_back(entry | borderStyled(ROUNDED, hex_color(s.info.color)));
        }
        if (servers.empty()) {
            rows.push_back(text(" no MCP servers yet") | dim);
            rows.push_back(text(" press n to create one") | dim);
        }
        return rows;
    };

    // ---------------- agent list ----------------
    auto server_color = [&](const std::string& name) {
        for (const auto& s : servers)
            if (s.info.name == name) return hex_color(s.info.color);
        return Color(Color::GrayLight);  // server was deleted
    };

    auto agents_body = [&] {
        Elements rows;
        for (int i = 0; i < static_cast<int>(agents.size()); ++i) {
            const auto& a = agents[i];
            bool shown = viewport == "agent:" + a.name;
            auto marker = text(shown ? " ● " : "   ") | color(Color::Green);
            auto unread = unread_counts.count(a.name) ? unread_counts.at(a.name) : 0;
            auto badge = unread ? text(" ✉ " + std::to_string(unread)) | color(Color::Yellow) | bold
                                : text("");
            Elements lines = {
                hbox({marker, text(a.name) | bold,
                      a.manager ? text(" manager") | color(Color::Magenta) : text(""), badge}),
                hbox({text("   "), text(shorten_home(a.dir)) | dim}),
                a.dead ? hbox({text("   "), text("✗ exited (" + std::to_string(a.exit_status) + ")  " +
                                                 config.key_kill + " to remove") | color(Color::Red)})
                       : hbox({text("   "), text(a.command) | color(Color::Cyan) | dim}),
            };
            std::optional<Color> outline;
            if (!a.servers.empty()) {
                Elements tags = {text("   ")};
                for (const auto& s : a.servers) {
                    auto c = server_color(s);
                    if (!outline) outline = c;
                    tags.push_back(text("■ " + s + " ") | color(c));
                }
                lines.push_back(hbox(tags));
            }
            auto entry = vbox(lines);
            if (i == selected) entry = entry | inverted;
            rows.push_back(entry | borderStyled(ROUNDED, outline.value_or(Color::GrayDark)));
        }
        if (agents.empty()) {
            rows.push_back(text(" no agents yet") | dim);
            rows.push_back(text(" press n to start one") | dim);
        }
        return rows;
    };

    auto machines_body = [&] {
        Elements rows;
        for (int i = 0; i < static_cast<int>(machines.size()); ++i) {
            const auto& machine = machines[i];
            bool current = machine.id == control_endpoint().target;
            auto title = machine.name.empty() ? machine.id : machine.name;
            Elements lines = {
                hbox({text(current ? " ● " : "   ") | color(Color::Green), text(title) | bold}),
            };
            if (!machine.name.empty()) lines.push_back(hbox({text("   "), text(machine.id) | dim}));
            auto entry = vbox(std::move(lines));
            if (i == selected_machine) entry = entry | inverted;
            rows.push_back(entry | borderRounded);
        }
        if (machines.empty()) rows.push_back(text(" no machine has joined") | dim);
        return rows;
    };

    auto list_view = Renderer([&](bool /*focused*/) {
        bool on_agents = tab == Tab::Agents;
        Elements help;
        if (choosing_machine) {
            help.push_back(text(" j/k move  enter select") | dim);
            help.push_back(text(control_endpoint().target.empty() ? " q quit" : " esc back  q quit") | dim);
        } else if (on_agents) {
            help.push_back(text(" j/k move  enter open") | dim);
            help.push_back(text(" n new  " + config.key_kill + " kill  s settings") | dim);
        } else {
            help.push_back(text(" j/k move  enter logs") | dim);
            help.push_back(text(" space start/stop  n new") | dim);
            help.push_back(text(" " + config.key_kill + " delete  s settings") | dim);
        }
        if (!choosing_machine) {
            help.push_back(text(" 1/2 tabs  " + config.key_detach + " back to list  q quit") | dim);
            if (control_endpoint().remote && machines.size() > 1) help.push_back(text(" m switch machine") | dim);
        }

        std::string driven = host_name;
        if (driven.empty()) {
            for (const auto& machine : machines)
                if (machine.id == control_endpoint().target) driven = machine.name.empty() ? machine.id : machine.name;
        }
        Elements rows = {
            text(" ai-orchestrator ") | bold | color(Color::Magenta) | center,
        };
        if (control_endpoint().remote) {
            if (choosing_machine) rows.push_back(text("which machine") | dim | center);
            else if (!driven.empty()) rows.push_back(text(driven) | dim | center);
        }
        if (!choosing_machine) rows.push_back(tab_bar());
        rows.push_back(separator());
        if (!control_error.empty()) rows.push_back(paragraph(control_error) | color(Color::Red));
        rows.push_back(vbox(choosing_machine ? machines_body() : (on_agents ? agents_body() : servers_body())) | yframe | flex);
        rows.push_back(separator());
        rows.push_back(vbox(help));
        return vbox(rows) | borderRounded;
    });

    auto move = [](int& sel, int count, int delta) {
        if (count) sel = (sel + delta + count) % count;
    };

    auto main_view = CatchEvent(list_view, [&](Event e) {
        bool on_agents = tab == Tab::Agents;
        int count = static_cast<int>(on_agents ? agents.size() : servers.size());
        int& sel = on_agents ? selected : selected_server;

        if (e == Event::Character('q')) {
            open_popup(show_quit);
            quit_btn->TakeFocus();
            return true;
        }
        if (choosing_machine) {
            int n = static_cast<int>(machines.size());
            if (e == Event::Character('j') || e == Event::ArrowDown) return move(selected_machine, n, 1), true;
            if (e == Event::Character('k') || e == Event::ArrowUp) return move(selected_machine, n, -1), true;
            if (e == Event::Escape && !control_endpoint().target.empty()) return choosing_machine = false, true;
            if ((e == Event::Return || e == Event::Character('l')) && n) {
                apply_target(machines[selected_machine].id);
                choosing_machine = false;
                refresh();
            }
            return true;
        }
        if (e == Event::Character('m') && control_endpoint().remote && machines.size() > 1) {
            for (int i = 0; i < static_cast<int>(machines.size()); ++i)
                if (machines[i].id == control_endpoint().target) selected_machine = i;
            choosing_machine = true;
            return true;
        }

        if (e == Event::Character('1')) return tab = Tab::Agents, true;
        if (e == Event::Character('2')) return tab = Tab::Servers, true;
        if (e == Event::Character('j') || e == Event::ArrowDown) return move(sel, count, 1), true;
        if (e == Event::Character('k') || e == Event::ArrowUp) return move(sel, count, -1), true;
        if (e == Event::Character('s')) return open_settings(), true;
        if (e == Event::Character('n')) {
            on_agents ? open_form() : open_server_form();
            return true;
        }

        if (on_agents) {
            if (e == Event::Return || e == Event::Character('l')) {
                if (count) {
                    auto name = agents[sel].name;
                    if (control.show_agent(name, true).empty() && control_endpoint().remote)
                        show_view("agent", name, true);
                }
                refresh();
                return true;
            }
            if (e == Event::Character(config.key_kill)) {
                if (count) {
                    control.delete_agent(agents[sel].name);
                }
                refresh();
                return true;
            }
            return false;
        }

        if (!count) return false;
        const auto& s = servers[sel].info;
        if (e == Event::Return || e == Event::Character('l')) {
            if (control.show_server(s.name).empty() && control_endpoint().remote) show_view("server", s.name, false);
            refresh();
            return true;
        }
        if (e == Event::Character(' ')) {
            std::string err = servers[sel].running ? control.stop_server(s.name) : control.start_server(s.name);
            refresh();
            if (!err.empty()) control_error = err;
            return true;
        }
        if (e == Event::Character(config.key_kill)) {
            control.delete_server(s.name);
            refresh();
            return true;
        }
        return false;
    });

    auto root = Modal(Modal(Modal(Modal(main_view, form_view, &show_form), settings_view, &show_settings),
                            server_form_view, &show_server_form),
                      quit_view, &show_quit);

    // Quitting goes through the q popup so agents and servers are stopped cleanly. Must run
    // inside the loop (after the screen configured the terminal and its signal handlers),
    // hence on the first frame, so Ctrl+C / Ctrl+Z / Ctrl+\ arrive as plain keys.
    bool signals_disabled = false;
    root = Renderer(root, [&, inner = root] {
        if (!signals_disabled) {
            Platform::disable_terminal_signals();
            signals_disabled = true;
        }
        return inner->Render();
    });

    std::atomic<bool> running = true;
    std::thread ticker([&] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            screen.Post([&] {
                if (!show_form && !show_settings && !show_server_form && !show_quit) refresh();
            });
            screen.PostEvent(Event::Custom);
        }
    });

    screen.Loop(root);
    running = false;
    ticker.join();
    return 0;
}

}  // namespace aio
