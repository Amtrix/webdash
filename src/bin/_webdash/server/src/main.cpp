#include <iostream>
#include <signal.h>
#include <fstream>
#include <pwd.h>
#include <grp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <array>
#include <set>
#include <optional>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <iomanip>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <webdash-config.hpp>
#include <webdash-config-task.hpp>
#include <webdash-utils.hpp>
#include <webdash-ipc.hpp>
#include <websocket.h>

#include "Suite.hpp"
#include "WebDashCore.hpp"
#include "HttpFileServer.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

const string _WEBDASH_PROJECT_NAME_ = "webdash-server";

static char pid_file_name[] = "/tmp/webdash-server.lock";
static int pid_fd = -1;

static std::optional<std::string> profile_get_string(const std::string& key) {
    const auto& profile_entries = WebDashCore::Get().GetKeyValuesFromRootProfile();
    for (const auto& entry : profile_entries) {
        if (WebDashUtils::GetWebDashJsonKey(entry) == key) {
            return entry.GetValue();
        }
    }
    return std::nullopt;
}

static std::vector<std::string> profile_get_string_list(const std::string& key_prefix) {
    // Collects all values whose key starts with key_prefix + "." (flattened array entries).
    // Array indices are stored as "[0]", "[1]", etc. by the profile parser.
    // e.g. key_prefix = "$#.code.editor.terminals" matches "$#.code.editor.terminals.[0]", etc.
    const std::string prefix = key_prefix + ".";
    std::vector<std::pair<int, std::string>> indexed;
    const auto& profile_entries = WebDashCore::Get().GetKeyValuesFromRootProfile();
    for (const auto& entry : profile_entries) {
        const std::string k = WebDashUtils::GetWebDashJsonKey(entry);
        if (k.rfind(prefix, 0) == 0) {
            const std::string suffix = k.substr(prefix.size());
            // suffix looks like "[0]", "[1]", etc.
            try {
                std::string num = suffix;
                if (num.size() >= 2 && num.front() == '[' && num.back() == ']') {
                    num = num.substr(1, num.size() - 2);
                }
                int idx = std::stoi(num);
                indexed.emplace_back(idx, entry.GetValue());
            } catch (...) {}
        }
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> result;
    result.reserve(indexed.size());
    for (auto& [_, v] : indexed) result.push_back(std::move(v));
    return result;
}

static bool is_under_root_dir(const fs::path& p, const fs::path& root) {
    try {
        auto pc = fs::weakly_canonical(p);
        auto rc = fs::weakly_canonical(root);
        auto rel = fs::relative(pc, rc);
        return !rel.empty() && *rel.begin() != "..";
    } catch (...) {
        return false;
    }
}

static std::string shell_escape_double_quotes(const std::string& s) {
    // For embedding inside a double-quoted shell string.
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static std::string shell_escape_single_quotes(const std::string& s) {
    // Safe to embed inside single quotes in a shell command.
    // ' becomes: '"'"'
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'') {
            out += "'\"'\"'";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

static std::string first_token(const std::string& cmd);

static std::string after_first_token(const std::string& cmd) {
    size_t i = 0;
    while (i < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[i]))) i++;
    while (i < cmd.size() && !std::isspace(static_cast<unsigned char>(cmd[i]))) i++;
    while (i < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[i]))) i++;
    return cmd.substr(i);
}

static bool looks_like_vscode_cmd(const std::string& editor_cmd) {
    const std::string bin = first_token(editor_cmd);
    const std::string bin_lc = to_lower(bin);
    return (bin_lc == "code") || ends_with_ci(bin, "code.exe") || (bin_lc.find("vscode") != std::string::npos);
}

static std::string first_token(const std::string& cmd) {
    // Minimal tokenization: first non-space chunk.
    // Good enough for typical editor commands like "code" or "/usr/bin/code --reuse-window".
    size_t i = 0;
    while (i < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[i]))) i++;
    size_t j = i;
    while (j < cmd.size() && !std::isspace(static_cast<unsigned char>(cmd[j]))) j++;
    return cmd.substr(i, j - i);
}

static std::optional<std::string> find_vscode_remote_cli_code_in_home(const std::string& home_path) {
    // VS Code Server installs a WSL-side CLI at:
    //   <home>/.vscode-server/bin/<commit>/bin/remote-cli/code
    // Prefer the newest commit directory.
    std::error_code ec;
    fs::path base = fs::path(home_path) / ".vscode-server" / "bin";
    if (!fs::exists(base, ec) || !fs::is_directory(base, ec)) {
        return std::nullopt;
    }

    fs::file_time_type best_time{};
    fs::path best_path;
    bool have_best = false;
    for (const auto& entry : fs::directory_iterator(base, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        fs::path candidate = entry.path() / "bin" / "remote-cli" / "code";
        if (!fs::exists(candidate, ec)) continue;
        auto t = fs::last_write_time(entry.path(), ec);
        if (ec) continue;
        if (!have_best || t > best_time) {
            best_time = t;
            best_path = candidate;
            have_best = true;
        }
    }
    if (!have_best) return std::nullopt;
    return best_path.string();
}

static std::optional<std::string> uid_to_username(uid_t uid) {
    struct passwd* pw = getpwuid(uid);
    if (!pw || !pw->pw_name) return std::nullopt;
    return std::string(pw->pw_name);
}

static std::optional<std::string> uid_to_home(uid_t uid) {
    struct passwd* pw = getpwuid(uid);
    if (!pw || !pw->pw_dir) return std::nullopt;
    return std::string(pw->pw_dir);
}

static int run_bash_lc_script_as_user(const std::string& script, const std::optional<std::string>& username) {
    // Run: bash -lc <script>
    // If username is set AND we are root, drop privileges to that user.
    pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        if (username.has_value() && ::geteuid() == 0) {
            struct passwd* pw = ::getpwnam(username->c_str());
            if (!pw) {
                _exit(127);
            }

            // Set group privileges first.
            (void)::initgroups(pw->pw_name, pw->pw_gid);
            (void)::setgid(pw->pw_gid);
            (void)::setuid(pw->pw_uid);

            if (pw->pw_dir) {
                ::setenv("HOME", pw->pw_dir, 1);
                (void)::chdir(pw->pw_dir);
            }
            ::setenv("USER", pw->pw_name, 1);
            ::setenv("LOGNAME", pw->pw_name, 1);
        }

        ::execl("/bin/bash", "bash", "-lc", script.c_str(), (char*)nullptr);
        _exit(127);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return status;
}

struct ScriptRunResult {
    int status = -1;
    std::string output;
};

static ScriptRunResult run_bash_lc_script_as_user_capture(const std::string& script, const std::optional<std::string>& username, size_t max_output_bytes = 64 * 1024) {
    // Run: bash -lc <script>
    // If username is set AND we are root, drop privileges to that user.
    // Capture combined stdout/stderr up to max_output_bytes.
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        return { -1, "" };
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return { -1, "" };
    }

    if (pid == 0) {
        // child
        (void)::dup2(pipefd[1], STDOUT_FILENO);
        (void)::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);

        if (username.has_value() && ::geteuid() == 0) {
            struct passwd* pw = ::getpwnam(username->c_str());
            if (!pw) {
                _exit(127);
            }

            (void)::initgroups(pw->pw_name, pw->pw_gid);
            (void)::setgid(pw->pw_gid);
            (void)::setuid(pw->pw_uid);

            if (pw->pw_dir) {
                ::setenv("HOME", pw->pw_dir, 1);
                (void)::chdir(pw->pw_dir);
            }
            ::setenv("USER", pw->pw_name, 1);
            ::setenv("LOGNAME", pw->pw_name, 1);
        }

        ::execl("/bin/bash", "bash", "-lc", script.c_str(), (char*)nullptr);
        _exit(127);
    }

    // parent
    ::close(pipefd[1]);

    std::string out;
    out.reserve(std::min<size_t>(max_output_bytes, 4096));
    std::array<char, 4096> buf{};
    while (out.size() < max_output_bytes) {
        const ssize_t n = ::read(pipefd[0], buf.data(), std::min<size_t>(buf.size(), max_output_bytes - out.size()));
        if (n <= 0) break;
        out.append(buf.data(), static_cast<size_t>(n));
    }
    ::close(pipefd[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return { -1, out };
    }
    return { status, out };
}

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out += ' ';
            continue;
        }
        out += s[i];
    }
    return out;
}

static std::unordered_map<std::string, std::string> parse_query_params(const std::string& full_url) {
    std::unordered_map<std::string, std::string> out;
    auto q = full_url.find('?');
    if (q == std::string::npos || q + 1 >= full_url.size()) {
        return out;
    }

    std::string qs = full_url.substr(q + 1);
    size_t pos = 0;
    while (pos < qs.size()) {
        auto amp = qs.find('&', pos);
        auto seg = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = seg.find('=');
        if (eq != std::string::npos) {
            auto k = seg.substr(0, eq);
            auto v = seg.substr(eq + 1);
            out[k] = url_decode(v);
        } else if (!seg.empty()) {
            out[seg] = "";
        }

        if (amp == std::string::npos) break;
        pos = amp + 1;
    }

    return out;
}

struct WidgetCommandDef {
    std::string name;
    std::string type;   // "widget" | "widget-command"
    std::string output; // path relative to config dir
    std::optional<std::string> widget_height;
};

struct WidgetProjectDef {
    std::string config_path;
    std::string widget_identifier;
    std::string project_dir_name;
    std::optional<std::string> project_name;
    std::vector<WidgetCommandDef> widget_ui_commands;
    std::vector<WidgetCommandDef> widget_commands;
};

static bool path_has_parent_reference(const fs::path& p) {
    for (const auto& part : p) {
        if (part == "..") return true;
    }
    return false;
}

static std::optional<WidgetProjectDef> parse_widget_project_config(const std::string& config_path) {
    try {
        std::ifstream in(config_path, std::ios::in);
        if (!in.is_open()) return std::nullopt;

        json j;
        in >> j;
        if (!j.is_object()) return std::nullopt;

        if (!j.contains("widget-identifier") || !j["widget-identifier"].is_string()) {
            return std::nullopt;
        }

        WidgetProjectDef out;
        out.config_path = config_path;
        out.widget_identifier = j["widget-identifier"].get<std::string>();
        if (out.widget_identifier.empty()) return std::nullopt;

        fs::path config_fs = fs::path(config_path);
        out.project_dir_name = config_fs.parent_path().filename().string();
        if (out.project_dir_name.empty()) {
            out.project_dir_name = out.widget_identifier;
        }

        if (j.contains("project-name") && j["project-name"].is_string()) {
            std::string pname = j["project-name"].get<std::string>();
            if (!pname.empty()) out.project_name = pname;
        }

        if (!j.contains("commands") || !j["commands"].is_array()) {
            return std::nullopt;
        }

        for (const auto& cmd : j["commands"]) {
            if (!cmd.is_object()) continue;
            if (!cmd.contains("name") || !cmd["name"].is_string()) continue;
            if (!cmd.contains("type") || !cmd["type"].is_string()) continue;
            if (!cmd.contains("output") || !cmd["output"].is_string()) continue;

            WidgetCommandDef def;
            def.name = cmd["name"].get<std::string>();
            def.type = cmd["type"].get<std::string>();
            def.output = cmd["output"].get<std::string>();

            auto try_parse_height = [&cmd]() -> std::optional<std::string> {
                if (!cmd.contains("widget-height")) return std::nullopt;

                const auto& h = cmd["widget-height"];
                if (h.is_number_integer()) {
                    const int v = h.get<int>();
                    if (v > 0) return std::to_string(v);
                    return std::nullopt;
                }

                if (h.is_string()) {
                    std::string s = h.get<std::string>();
                    if (!s.empty()) return s;
                }

                return std::nullopt;
            };
            def.widget_height = try_parse_height();

            if (def.name.empty() || def.output.empty()) continue;
            if (def.type == "widget") {
                out.widget_ui_commands.push_back(def);
            } else if (def.type == "widget-command") {
                out.widget_commands.push_back(def);
            }
        }

        if (out.widget_ui_commands.empty() && out.widget_commands.empty()) {
            return std::nullopt;
        }

        return out;
    } catch (...) {
        return std::nullopt;
    }
}

static std::vector<WidgetProjectDef> collect_widget_projects(Suite& suite) {
    std::vector<WidgetProjectDef> result;
    auto paths = suite.GetRegisteredConfigPaths();
    for (const auto& config_path : paths) {
        auto parsed = parse_widget_project_config(config_path);
        if (parsed.has_value()) {
            result.push_back(std::move(parsed.value()));
        }
    }
    return result;
}

static std::optional<WidgetProjectDef> find_widget_project(Suite& suite, const std::string& widget_identifier) {
    auto projects = collect_widget_projects(suite);
    for (const auto& p : projects) {
        if (p.widget_identifier == widget_identifier) {
            return p;
        }
    }
    return std::nullopt;
}

static std::optional<WidgetCommandDef> find_widget_command(const WidgetProjectDef& project,
                                                           const std::string& command_name,
                                                           const std::string& expected_type)
{
    const auto* source = &project.widget_commands;
    if (expected_type == "widget") {
        source = &project.widget_ui_commands;
    }

    for (const auto& c : *source) {
        if (c.name == command_name) {
            return c;
        }
    }
    return std::nullopt;
}

static json build_network_shared_paths_json(Suite& suite) {
    json arr = json::array();
    auto shared_paths = suite.GetNetworkSharedPaths();

    for (const auto& [alias, directory] : shared_paths) {
        json item;
        item["name"] = alias;
        item["directory"] = directory;
        item["urlPrefix"] = "/" + alias;
        arr.push_back(item);
    }

    return arr;
}

static std::optional<fs::path> sanitize_relative_output_path(const std::string& output_path) {
    if (output_path.empty()) return std::nullopt;

    fs::path rel = fs::path(output_path).lexically_normal();
    if (rel.is_absolute()) return std::nullopt;
    if (path_has_parent_reference(rel)) return std::nullopt;

    return rel;
}

static json execute_widget_task(Suite& suite,
                                const WidgetProjectDef& project,
                                const WidgetCommandDef& command,
                                const json& payload)
{
    json resp;
    resp["ok"] = false;
    resp["project"] = project.widget_identifier;
    resp["command"] = command.name;

    auto reg = suite.GetRegisteredConfig(project.config_path);
    if (!reg.has_value()) {
        resp["error"] = "project is no longer registered";
        return resp;
    }

    auto rel_output = sanitize_relative_output_path(command.output);
    if (!rel_output.has_value()) {
        resp["error"] = "invalid command output path";
        return resp;
    }

    std::vector<std::string> extra_args;
    if (command.type == "widget-command") {
        json effective_payload = json::object();
        if (payload.is_object()) {
            effective_payload = payload;
        } else if (!payload.is_null()) {
            effective_payload["value"] = payload;
        }

        effective_payload["__webdash"]["project"] = project.widget_identifier;
        effective_payload["__webdash"]["networkSharedPaths"] = build_network_shared_paths_json(suite);
        extra_args.push_back(effective_payload.dump());
    } else if (!payload.is_null()) {
        extra_args.push_back(payload.dump());
    }

    WebDashType::RunConfig run_cfg;
    run_cfg.skip_frequency_check = true;
    run_cfg.run_as_uid = reg->run_as_uid;
    run_cfg.run_as_gid = reg->run_as_gid;
    run_cfg.run_as_home = reg->run_as_home;

    auto run_results = reg->config.Run(command.name, run_cfg, extra_args);
    if (run_results.empty()) {
        resp["error"] = "task did not execute (missing command?)";
        return resp;
    }

    bool any_failure = false;
    for (const auto& rr : run_results) {
        if (!rr.skipped && rr.return_code != 0) {
            any_failure = true;
            break;
        }
    }
    if (any_failure) {
        resp["error"] = "task execution failed";
        return resp;
    }

    fs::path config_dir = fs::path(project.config_path).parent_path();
    fs::path source = config_dir / *rel_output;

    std::error_code ec;
    fs::path source_canon = fs::weakly_canonical(source, ec);
    if (ec) {
        resp["error"] = "command output does not exist";
        return resp;
    }

    fs::path config_dir_canon = fs::weakly_canonical(config_dir, ec);
    if (ec) {
        resp["error"] = "failed to access config directory";
        return resp;
    }

    auto rel_from_cfg = fs::relative(source_canon, config_dir_canon, ec);
    if (ec || rel_from_cfg.empty() || *rel_from_cfg.begin() == "..") {
        resp["error"] = "output path escapes project directory";
        return resp;
    }

    if (!fs::exists(source_canon) || !fs::is_regular_file(source_canon)) {
        resp["error"] = "output file was not generated";
        return resp;
    }

    fs::path storage_dir = WebDashCore::Get().GetWebDashRootDirectory() /
                           "app-persistent" / "data" / project.project_dir_name;
    fs::path persisted_output = storage_dir / rel_from_cfg;

    fs::create_directories(persisted_output.parent_path(), ec);
    if (ec) {
        resp["error"] = "failed to prepare persistent output directory";
        return resp;
    }

    fs::copy_file(source_canon, persisted_output, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        resp["error"] = "failed to persist output file";
        return resp;
    }

    std::ifstream in(persisted_output, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        resp["error"] = "failed to read persisted output file";
        return resp;
    }

    std::stringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();

    resp["ok"] = true;
    resp["output"] = rel_from_cfg.generic_string();
    resp["persistedPath"] = persisted_output.string();
    resp["content"] = content;

    try {
        resp["data"] = json::parse(content);
    } catch (...) {
        // Not JSON output; callers can still use "content".
    }

    return resp;
}

// Returns:
//   nullopt => launched
//   "no-editor" => no configured editor
//   otherwise => error message
static std::optional<std::string> open_dir_in_editor(
    const fs::path& dir_path,
    const std::optional<uid_t>& run_as_uid,
    const std::optional<std::string>& run_as_home
) {
    const std::string key_editor_base = std::string(WebDashConst::Internal::kJsonKeyPrefix) + ".code.editor";
    const std::string key_editor_name = key_editor_base + ".name";
    const std::string key_editor_user = key_editor_base + ".user";
    const std::string key_host        = key_editor_base + ".host";
    const std::string key_distro      = key_editor_base + ".distro";
    const std::string key_debug       = key_editor_base + ".debug";
    const std::string key_source_env  = std::string(WebDashConst::Internal::kJsonKeyPrefix) + ".source-environment-file";

    // Support both:
    //  - "code": { "editor": "code --reuse-window" }
    //  - "code": { "editor": { "name": "code", "host": "wsl" } }
    auto editor_opt = profile_get_string(key_editor_name);
    if (!editor_opt || editor_opt->empty()) {
        editor_opt = profile_get_string(key_editor_base);
    }
    if (!editor_opt || editor_opt->empty()) {
        return std::string("no-editor");
    }

    const std::string host = profile_get_string(key_host).value_or("");
    const std::string distro = profile_get_string(key_distro).value_or("");

    auto truthy = [](const std::string& s) {
        const std::string v = to_lower(s);
        return (v == "1" || v == "true" || v == "yes" || v == "on" || v == "y");
    };
    const bool editor_debug = ([](const std::string& key_debug, const auto& truthy_fn) {
        if (const char* env = std::getenv("WEBDASH_EDITOR_DEBUG")) {
            if (truthy_fn(env)) return true;
        }
        if (auto v = profile_get_string(key_debug); v.has_value()) {
            return truthy_fn(*v);
        }
        return false;
    })(key_debug, truthy);

    std::string source_env;
    if (auto src = profile_get_string(key_source_env); src && !src->empty()) {
        // Only use it if it exists; otherwise silently ignore.
        std::error_code ec;
        if (fs::exists(*src, ec)) {
            source_env = *src;
        }
    }

    const std::string editor_cmd = *editor_opt;

    // Choose which user context to use for launching.
    // webdash-server runs as root; we want the same behavior as `code .` in the user's WSL shell.
    std::optional<std::string> username;
    std::string home_path;
    if (auto u = profile_get_string(key_editor_user); u && !u->empty()) {
        // Explicit override: use this user for editor launches.
        username = *u;
        struct passwd* pw = getpwnam(username->c_str());
        if (pw && pw->pw_dir) {
            home_path = pw->pw_dir;
        }
    } else if (run_as_uid.has_value()) {
        username = uid_to_username(*run_as_uid);
        if (run_as_home.has_value()) home_path = *run_as_home;
        if (home_path.empty()) {
            if (auto h = uid_to_home(*run_as_uid); h.has_value()) home_path = *h;
        }
    }
    if (home_path.empty()) {
        if (const char* env_home = std::getenv("HOME")) home_path = env_home;
    }

    // If the user wants a WSL-context open, prefer the WSL-side VS Code remote CLI.
    // This mirrors `code .` inside WSL and avoids accidentally calling /mnt/c/.../code.
    std::string editor_call = editor_cmd;
    std::string explicit_bin_path;
    if (host == "wsl" && looks_like_vscode_cmd(editor_cmd) && !home_path.empty()) {
        const std::string rest = after_first_token(editor_cmd);
        auto remote_cli = find_vscode_remote_cli_code_in_home(home_path);
        if (remote_cli.has_value()) {
            explicit_bin_path = *remote_cli;
            editor_call = "\"" + shell_escape_double_quotes(*remote_cli) + "\"";
            if (!rest.empty()) editor_call += " " + rest;
        }
    }

    // Make VS Code open a NEW window (avoid reusing an existing window/folder).
    if (host == "wsl" && looks_like_vscode_cmd(editor_cmd)) {
        if (editor_call.find("--new-window") == std::string::npos && editor_call.find(" -n") == std::string::npos) {
            editor_call += " --new-window";
        }
    }

    // Validate editor availability (using user shell if possible).
    {
        std::string check_script;
        std::string missing_msg;
        if (!explicit_bin_path.empty()) {
            const std::string p = shell_escape_single_quotes(explicit_bin_path);
            check_script = "test -x '\''" + p + "'\''";
            missing_msg = std::string("VS Code remote CLI not executable: ") + explicit_bin_path;
        } else {
            const std::string bin = first_token(editor_call);
            if (bin.empty()) return std::string("no-editor");
            const std::string b = shell_escape_single_quotes(bin);
            check_script = "command -v '\''" + b + "'\'' >/dev/null 2>&1";
            missing_msg = std::string("editor not found: ") + bin;
        }

        if (!source_env.empty()) {
            const std::string src = shell_escape_single_quotes(source_env);
            check_script = "source '\''" + src + "'\'' >/dev/null 2>&1; " + check_script;
        }

        // Run check as the target user so PATH/HOME match.
        const int st = run_bash_lc_script_as_user(check_script, username);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            if (!source_env.empty()) {
                return missing_msg + " (even after sourcing " + source_env + ")";
            }
            return missing_msg + " (not found/executable)";
        }
    }

    const std::string dir = shell_escape_double_quotes(dir_path.string());
    const std::string dir_sq = shell_escape_single_quotes(dir_path.string());

    std::string script;

    const bool is_vscode = looks_like_vscode_cmd(editor_cmd);
    if (is_vscode && editor_debug) {
        const std::string prof_user = profile_get_string(key_editor_user).value_or("");
        const std::string prof_distro = profile_get_string(key_distro).value_or("");
        const std::string target_user = username.value_or("");
        const std::string target_home = home_path;
        script += "echo \"[webdash] profile code.editor.user=" + shell_escape_double_quotes(prof_user) + "\"; ";
        script += "echo \"[webdash] profile code.editor.distro=" + shell_escape_double_quotes(prof_distro) + "\"; ";
        script += "echo \"[webdash] resolved target user=" + shell_escape_double_quotes(target_user) + "\"; ";
        script += "echo \"[webdash] resolved home=" + shell_escape_double_quotes(target_home) + "\"; ";
        script += "echo \"[webdash] editor_call=" + shell_escape_double_quotes(editor_call) + "\"; ";
        if (!explicit_bin_path.empty()) {
            script += "echo \"[webdash] explicit_bin_path=" + shell_escape_double_quotes(explicit_bin_path) + "\"; ";
        }
    }

    // WSL services started via systemd often don't have the interop env that interactive shells have.
    // Without WSL_INTEROP, VS Code's WSL-side CLI may not be able to talk back to the Windows-side UI.
    if (host == "wsl" && is_vscode) {
        if (!distro.empty()) {
            const std::string d = shell_escape_single_quotes(distro);
            script += "export WSL_DISTRO_NAME='" + d + "'; ";
        }
        script += "if [ -z \"$WSL2_GUI_APPS_ENABLED\" ]; then export WSL2_GUI_APPS_ENABLED=1; fi; ";
    }

    if (!source_env.empty()) {
        const std::string src = shell_escape_single_quotes(source_env);
        script += "source '\''" + src + "'\'' >/dev/null 2>&1; ";
    }
    // For VS Code: run with a short timeout so the API stays responsive.
    // For other editors: background to avoid blocking the server.
    if (is_vscode) {
        // Capture output (for error reporting and to detect known "silent no-op" cases).
        // Prefer Windows-side `code --remote wsl+<distro>` (when host==wsl) to avoid WSL remote-cli no-ops.
        script += "set +e; out=''; rc=0; ";

        if (host == "wsl" && !distro.empty()) {
            script += "if [ -x /mnt/c/Windows/System32/cmd.exe ] && /mnt/c/Windows/System32/cmd.exe /c where code >/dev/null 2>&1; then ";
            const std::string d = shell_escape_single_quotes(distro);
            script += "out=$({ timeout 6s /mnt/c/Windows/System32/cmd.exe /c code --remote wsl+" + d + " --new-window '" + dir_sq + "'; } 2>&1); rc=$?; ";
            script += "else ";
        }

        if (host == "wsl") {
            // Only require WSL_INTEROP if we fall back to the WSL-side remote CLI.
            // /run/WSL/1_interop is a symlink on some systems; -S will fail on the symlink itself.
            script += "if [ -z \"$WSL_INTEROP\" ] || [ ! -S \"$WSL_INTEROP\" ]; then "
                      "cand=; "
                      "if [ -e /run/WSL/1_interop ]; then cand=$(readlink -f /run/WSL/1_interop 2>/dev/null || true); fi; "
                      "if [ -n \"$cand\" ] && [ -S \"$cand\" ]; then WSL_INTEROP=$cand; fi; "
                      "if [ -z \"$WSL_INTEROP\" ] || [ ! -S \"$WSL_INTEROP\" ]; then "
                      "for s in /run/WSL/*_interop; do if [ -S \"$s\" ]; then WSL_INTEROP=$s; break; fi; done; "
                      "fi; "
                      "export WSL_INTEROP; "
                      "fi; ";
            script += "if [ -z \"$WSL_INTEROP\" ] || [ ! -S \"$WSL_INTEROP\" ]; then echo 'WSL_INTEROP is not set to a socket (need a valid /run/WSL/*_interop)'; exit 78; fi; ";
        }

        script += "out=$({ timeout 6s " + editor_call + " \"" + dir + "\"; } 2>&1); rc=$?; ";

        if (host == "wsl" && !distro.empty()) {
            script += "fi; ";
        }

        script += "if [ -n \"$out\" ]; then echo \"$out\"; fi; exit $rc";
    } else {
        script += "nohup " + editor_call + " \"" + dir + "\" >/dev/null 2>&1 &";
    }

    ScriptRunResult r;
    if (is_vscode) {
        r = run_bash_lc_script_as_user_capture(script, username);
    } else {
        r.status = run_bash_lc_script_as_user(script, username);
        r.output.clear();
    }

    auto looks_like_vscode_noop = [](const std::string& s) {
        const std::string sl = to_lower(s);
        return sl.find("command is only available in wsl") != std::string::npos;
    };

    auto looks_like_windows_code_missing = [](const std::string& s) {
        const std::string sl = to_lower(s);
        return sl.find("is not recognized as an internal or external command") != std::string::npos;
    };

    std::string captured_out = r.output;
    if (!((WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0)) || (is_vscode && (looks_like_vscode_noop(captured_out) || looks_like_windows_code_missing(captured_out)))) {
        int exit_code = -1;
        if (WIFEXITED(r.status)) {
            exit_code = WEXITSTATUS(r.status);
        } else if (WIFSIGNALED(r.status)) {
            // Encode signals as 128+signum (common convention)
            exit_code = 128 + WTERMSIG(r.status);
        }

        std::string msg = "failed to launch editor";
        if (editor_debug) {
            if (exit_code >= 0) {
                msg += " (exit=" + std::to_string(exit_code) + ", status=" + std::to_string(r.status) + ")";
            } else {
                msg += " (status=" + std::to_string(r.status) + ")";
            }
        }

        // Surface helpful output, but keep it concise.
        if (!captured_out.empty()) {
            std::string out = captured_out;
            if (out.size() > 2000) out.resize(2000);
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            if (!out.empty()) {
                msg += ": ";
                msg += out;
            }
        } else if (editor_debug) {
            msg += ": (no output)";
        }

        // Make common failure modes clearer.
        if (is_vscode && looks_like_windows_code_missing(captured_out)) {
            msg += " (Windows 'code' command not found on PATH)";
        }
        if (is_vscode && looks_like_vscode_noop(captured_out)) {
            msg += " (VS Code remote CLI no-op)";
        }

        return msg;
    }
    return std::nullopt;
}

// ── Terminal launch ──────────────────────────────────────────────────────
// Launches a terminal (wsl, powershell, cmd) with cwd set to `dir_path`.
// Returns nullopt on success or an error string.
static std::optional<std::string> open_terminal(
    const std::string& terminal_type,
    const fs::path& dir_path,
    const std::optional<uid_t>& run_as_uid,
    const std::optional<std::string>& run_as_home
) {
    const std::string key_editor_base = std::string(WebDashConst::Internal::kJsonKeyPrefix) + ".code.editor";
    const std::string key_editor_user = key_editor_base + ".user";
    const std::string key_distro      = key_editor_base + ".distro";
    const std::string key_host        = key_editor_base + ".host";
    const std::string key_source_env  = std::string(WebDashConst::Internal::kJsonKeyPrefix) + ".source-environment-file";

    const std::string host   = profile_get_string(key_host).value_or("");
    const std::string distro = profile_get_string(key_distro).value_or("");

    std::string source_env;
    if (auto src = profile_get_string(key_source_env); src && !src->empty()) {
        std::error_code ec;
        if (fs::exists(*src, ec)) {
            source_env = *src;
        }
    }

    // Resolve user context.
    std::optional<std::string> username;
    std::string home_path;
    if (auto u = profile_get_string(key_editor_user); u && !u->empty()) {
        username = *u;
        struct passwd* pw = getpwnam(username->c_str());
        if (pw && pw->pw_dir) home_path = pw->pw_dir;
    } else if (run_as_uid.has_value()) {
        username = uid_to_username(*run_as_uid);
        if (run_as_home.has_value()) home_path = *run_as_home;
        if (home_path.empty()) {
            if (auto h = uid_to_home(*run_as_uid); h.has_value()) home_path = *h;
        }
    }
    if (home_path.empty()) {
        if (const char* env_home = std::getenv("HOME")) home_path = env_home;
    }

    const std::string dir_escaped_dq = shell_escape_double_quotes(dir_path.string());
    const std::string dir_escaped_sq = shell_escape_single_quotes(dir_path.string());

    const std::string type_lc = to_lower(terminal_type);

    std::string script;

    if (type_lc == "wsl") {
        // Open a WSL shell (via Windows Terminal or wsl.exe) in the project directory.
        if (host == "wsl") {
            if (!distro.empty()) {
                const std::string d = shell_escape_single_quotes(distro);
                script += "export WSL_DISTRO_NAME='" + d + "'; ";
            }
            script += "if [ -z \"$WSL2_GUI_APPS_ENABLED\" ]; then export WSL2_GUI_APPS_ENABLED=1; fi; ";
        }

        if (!source_env.empty()) {
            const std::string src = shell_escape_single_quotes(source_env);
            script += "source '" + src + "' >/dev/null 2>&1; ";
        }

        // WSL_INTEROP recovery.
        if (host == "wsl") {
            script += "if [ -z \"$WSL_INTEROP\" ] || [ ! -S \"$WSL_INTEROP\" ]; then "
                      "cand=; "
                      "if [ -e /run/WSL/1_interop ]; then cand=$(readlink -f /run/WSL/1_interop 2>/dev/null || true); fi; "
                      "if [ -n \"$cand\" ] && [ -S \"$cand\" ]; then WSL_INTEROP=$cand; fi; "
                      "if [ -z \"$WSL_INTEROP\" ] || [ ! -S \"$WSL_INTEROP\" ]; then "
                      "for s in /run/WSL/*_interop; do if [ -S \"$s\" ]; then WSL_INTEROP=$s; break; fi; done; "
                      "fi; "
                      "export WSL_INTEROP; "
                      "fi; ";
            script += "if [ -z \"$WSL_INTEROP\" ] || [ ! -S \"$WSL_INTEROP\" ]; then echo 'WSL_INTEROP is not set to a socket'; exit 78; fi; ";
        }

        // Launch a WSL shell in Windows Terminal (wt.exe) if available, else fall back to wsl.exe.
        // Convert the WSL path to a Windows path so wt.exe/wsl.exe can understand it.
        script += "set +e; ";
        script += "windir=$(wslpath -w '" + dir_escaped_sq + "' 2>/dev/null); ";
        script += "if [ -z \"$windir\" ]; then echo 'Failed to convert path to Windows format'; exit 1; fi; ";
        if (!distro.empty()) {
            const std::string d = shell_escape_single_quotes(distro);
            // Try wt.exe first (opens a nice tabbed terminal).
            script += "if /mnt/c/Windows/System32/cmd.exe /c where wt >nul 2>&1; then ";
            script += "nohup /mnt/c/Windows/System32/cmd.exe /c start wt.exe -p \"" + shell_escape_double_quotes(distro) + "\" -d \"$windir\" >/dev/null 2>&1 & ";
            script += "else ";
            script += "nohup /mnt/c/Windows/System32/cmd.exe /c start wsl.exe -d " + d + " --cd \"$windir\" >/dev/null 2>&1 & ";
            script += "fi";
        } else {
            script += "nohup /mnt/c/Windows/System32/cmd.exe /c start wsl.exe --cd \"$windir\" >/dev/null 2>&1 &";
        }

    } else if (type_lc == "powershell") {
        // Launch PowerShell on Windows side, cd to the project directory.
        // Convert WSL path to Windows path.
        script += "set +e; ";
        script += "windir=$(wslpath -w '" + dir_escaped_sq + "' 2>/dev/null); ";
        script += "if [ -z \"$windir\" ]; then echo 'Failed to convert path to Windows format'; exit 1; fi; ";
        script += "nohup /mnt/c/Windows/System32/cmd.exe /c start powershell.exe -NoExit -Command \"cd '$windir'\" >/dev/null 2>&1 &";

    } else {
        return std::string("unsupported terminal type: ") + terminal_type;
    }

    ScriptRunResult r;
    r = run_bash_lc_script_as_user_capture(script, username);

    if (WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0) {
        return std::nullopt;
    }

    std::string msg = "failed to launch " + type_lc + " terminal";
    if (!r.output.empty()) {
        std::string out = r.output;
        if (out.size() > 2000) out.resize(2000);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (!out.empty()) {
            msg += ": ";
            msg += out;
        }
    }
    return msg;
}

// On how to use JSON:
// https://github.com/nlohmann/json


/* Signal Handler for SIGINT */
void sigintHandler(int sig_num)
{
    if (sig_num == SIGINT) {
        cout << "Caught SIGINT" << endl;
        websocket::Stop();

        /* Unlock and close lockfile */
        if (pid_fd != -1) {
			flock(pid_fd, LOCK_UN);
			close(pid_fd);
		}

        /* Try to delete lockfile */
		unlink(pid_file_name);

        signal(SIGINT, SIG_DFL);
    }
}

// daemonize() is currently unused - kept for future use when daemon mode is re-enabled.
// To re-enable, uncomment this function and the call in execute_main().
#if 0
static void daemonize()
{
	pid_t pid = 0;
	int fd;

	// Fork off the parent process
	pid = fork();

	// An error occurred
	if (pid < 0) {
        printf("fork(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// Success: Let the parent terminate
	if (pid > 0) {
        printf("Terminate parent #1: %s\n", strerror(errno));
		exit(EXIT_SUCCESS);
	}

	// On success: The child process becomes session leader
	if (setsid() < 0) {
        printf("setsid(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// Ignore signal sent from child to parent process
	signal(SIGCHLD, SIG_IGN);

	// Fork off for the second time
	pid = fork();

	// An error occurred
	if (pid < 0) {
        printf("child fork(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// Success: Let the parent terminate
	if (pid > 0) {
        printf("Terminate parent #2: %s\n", strerror(errno));
		exit(EXIT_SUCCESS);
	}

	// Set new file permissions
	umask(0);

    printf("Daemon continues alone.\n");

	// Change the working directory to the root directory
	// or another appropriated directory
	if (chdir("/") != 0) {
        perror("chdir() to / failed");
        exit(EXIT_FAILURE);
    }

	// Close all open file descriptors
	for (fd = sysconf(_SC_OPEN_MAX); fd >= 0; fd--) {
		close(fd);
	}

	// Reopen stdin (fd = 0), stdout (fd = 1), stderr (fd = 2)
	stdin = fopen("/dev/null", "r");
	stdout = fopen("/tmp/webdash-server.out", "w+");
	stderr = fopen("/tmp/webdash-server.err", "w+");

    freopen("/dev/null", "r", stdin);
    freopen("/tmp/webdash-server.out", "w+", stdout);
    freopen("/tmp/webdash-server.err", "w+", stderr);

	// Try to write PID of daemon to lockfile
	if (pid_file_name != NULL) {
		char str[256];
		pid_fd = open(pid_file_name, O_RDWR|O_CREAT, 0640);
		if (pid_fd < 0) {
			// Can't open lockfile
            perror("pid_fd = open() failed");
			exit(EXIT_FAILURE);
		}
		if (lockf(pid_fd, F_TLOCK, 0) < 0) {
			// Can't lock file
            perror("locf() failed");
			exit(EXIT_FAILURE);
		}
		// Get current PID
		sprintf(str, "%d\n", getpid());
		// Write PID to lockfile
		if (write(pid_fd, str, strlen(str)) <= 0) {
            cerr << "Error writing pid to lockfile." << endl;
        }
	}
}
#endif

// Kill any existing webdash-server process and take over the PID file
static void ensure_single_instance() {
    // Try to read existing PID from lockfile
    ifstream pid_in(pid_file_name);
    if (pid_in.good()) {
        pid_t old_pid = 0;
        pid_in >> old_pid;
        pid_in.close();

        if (old_pid > 0 && old_pid != getpid()) {
            // Check if process exists
            if (kill(old_pid, 0) == 0) {
                cout << "Killing existing webdash-server (PID " << old_pid << ")..." << endl;
                // Send SIGTERM first for graceful shutdown
                if (kill(old_pid, SIGTERM) == 0) {
                    // Wait a bit for graceful shutdown
                    usleep(500000); // 500ms
                    // If still running, force kill
                    if (kill(old_pid, 0) == 0) {
                        cout << "Process didn't exit, sending SIGKILL..." << endl;
                        kill(old_pid, SIGKILL);
                        usleep(100000); // 100ms
                    }
                }
            }
        }
    }

    // Now open and lock the PID file
    pid_fd = open(pid_file_name, O_RDWR | O_CREAT | O_TRUNC, 0640);
    if (pid_fd < 0) {
        cerr << "Failed to open PID file: " << strerror(errno) << endl;
        exit(EXIT_FAILURE);
    }

    // Try to get exclusive lock (non-blocking)
    if (flock(pid_fd, LOCK_EX | LOCK_NB) < 0) {
        cerr << "Failed to lock PID file (another instance starting?): " << strerror(errno) << endl;
        close(pid_fd);
        exit(EXIT_FAILURE);
    }

    // Write our PID
    string pid_str = to_string(getpid()) + "\n";
    if (write(pid_fd, pid_str.c_str(), pid_str.length()) <= 0) {
        cerr << "Failed to write PID to lockfile: " << strerror(errno) << endl;
    }

    // Keep pid_fd open to maintain the lock
    cout << "PID file created: " << pid_file_name << " (PID " << getpid() << ")" << endl;
}

int execute_main()
{
    cout << "Starting server..." << endl;

    // Ensure only one instance runs - kill any existing server first
    ensure_single_instance();

    // Set handler for SIGINT.
    signal(SIGINT, sigintHandler);

    WebDashCore::Get().Log(WebDashType::LogType::INFO, "main called.");

    Suite suite;
    suite.LoadSaved();

    //
    // Spawns a new thread that performs periodic execution of all configs.
    //

    suite.Run();

    //
    // Start the static file server if a port is configured.
    // Serves files from project directories that have "network-shared": true.
    //

    std::unique_ptr<HttpFileServer> file_server;
    {
        // Look up the static-file-server port from the profile's "ports" section
        const auto& profile_entries = WebDashCore::Get().GetKeyValuesFromRootProfile();
        int static_port = 0;

        const string target_key = string(WebDashConst::Internal::kJsonKeyPrefix) +
                                  ".ports." +
                                  string(WebDashConst::Public::kProfilePortStaticFileServer);

        for (const auto& entry : profile_entries) {
            if (WebDashUtils::GetWebDashJsonKey(entry) == target_key) {
                try {
                    static_port = stoi(entry.GetValue());
                } catch (...) {
                    WebDashCore::Get().Log(WebDashType::LogType::ERR,
                        "Invalid static-file-server port in profile: " + entry.GetValue());
                }
                break;
            }
        }

        if (static_port > 0) {
            // API handler for the portal dashboard — runs inside the HTTP server threads.
            ApiHandler api_handler = [&suite](const string& url_path) -> std::optional<string> {
                if (url_path == "/_api/list-configs") {
                    return suite.GetRegisteredConfigsJsonDump();
                }
                if (url_path == "/_api/recent-actions") {
                    auto actions = suite.GetRecentActions(20);
                    json arr = json::array();
                    for (const auto& a : actions) {
                        json item;
                        item["timestamp"]   = a.timestamp;
                        item["config"]      = a.config_path;
                        item["task"]        = a.task_name;
                        item["return_code"] = a.return_code;
                        arr.push_back(item);
                    }
                    return arr.dump();
                }
                if (url_path == "/_api/network-shared-paths") {
                    return build_network_shared_paths_json(suite).dump();
                }
                if (url_path == "/_api/widgets") {
                    json out = json::array();
                    auto widget_projects = collect_widget_projects(suite);

                    for (const auto& project : widget_projects) {
                        json project_json;
                        project_json["project"] = project.widget_identifier;
                        project_json["projectDir"] = project.project_dir_name;
                        project_json["config"] = project.config_path;
                        if (project.project_name.has_value()) {
                            project_json["projectName"] = project.project_name.value();
                        }

                        json command_arr = json::array();
                        for (const auto& cmd : project.widget_commands) {
                            command_arr.push_back(cmd.name);
                        }
                        project_json["commands"] = command_arr;

                        json widgets = json::array();
                        for (const auto& ui_cmd : project.widget_ui_commands) {
                            json widget_item;
                            widget_item["name"] = ui_cmd.name;
                            widget_item["output"] = ui_cmd.output;
                            if (ui_cmd.widget_height.has_value()) {
                                widget_item["height"] = ui_cmd.widget_height.value();
                            }

                            json exec_result = execute_widget_task(suite, project, ui_cmd, json());
                            widget_item["ok"] = exec_result.value("ok", false);
                            if (exec_result.value("ok", false)) {
                                widget_item["html"] = exec_result.value("content", "");
                                widget_item["persistedPath"] = exec_result.value("persistedPath", "");
                            } else {
                                widget_item["error"] = exec_result.value("error", "failed to load widget");
                            }

                            widgets.push_back(widget_item);
                        }

                        project_json["widgets"] = widgets;
                        out.push_back(project_json);
                    }

                    return out.dump();
                }
                if (url_path.rfind("/_api/widget-command", 0) == 0) {
                    json resp;
                    auto params = parse_query_params(url_path);

                    const std::string project = params.count("project") ? params["project"] : "";
                    const std::string command = params.count("command") ? params["command"] : "";
                    const std::string payload_raw = params.count("payload") ? params["payload"] : "";

                    if (project.empty() || command.empty()) {
                        resp["ok"] = false;
                        resp["error"] = "missing project or command parameter";
                        return resp.dump();
                    }

                    auto widget_project = find_widget_project(suite, project);
                    if (!widget_project.has_value()) {
                        resp["ok"] = false;
                        resp["error"] = "unknown widget project";
                        return resp.dump();
                    }

                    auto cmd = find_widget_command(widget_project.value(), command, "widget-command");
                    if (!cmd.has_value()) {
                        resp["ok"] = false;
                        resp["error"] = "unknown widget command";
                        return resp.dump();
                    }

                    json payload = json();
                    if (!payload_raw.empty()) {
                        try {
                            payload = json::parse(payload_raw);
                        } catch (...) {
                            payload = payload_raw;
                        }
                    }

                    resp = execute_widget_task(suite, widget_project.value(), cmd.value(), payload);
                    return resp.dump();
                }
                if (url_path == "/_api/git-status") {
                    // Collect unique git repo roots from registered config paths
                    json arr = json::array();
                    std::set<std::string> seen_repos;
                    auto configs_json = json::parse(suite.GetRegisteredConfigsJsonDump());

                    for (const auto& item : configs_json) {
                        std::string cfg_path = item["path"].get<std::string>();
                        // Config path is .../webdash.config.json — get its directory
                        auto dir = std::filesystem::path(cfg_path).parent_path();

                        // Walk up to find .git directory
                        std::string repo_root;
                        for (auto d = dir; !d.empty() && d != d.root_path(); d = d.parent_path()) {
                            if (std::filesystem::exists(d / ".git")) {
                                repo_root = d.string();
                                break;
                            }
                        }
                        if (repo_root.empty() || seen_repos.count(repo_root)) continue;
                        seen_repos.insert(repo_root);

                        // Helper: run a command and capture stdout
                        auto run_cmd = [](const std::string& cmd) -> std::string {
                            std::string result;
                            FILE* pipe = popen(cmd.c_str(), "r");
                            if (!pipe) return "";
                            char buf[256];
                            while (fgets(buf, sizeof(buf), pipe)) result += buf;
                            pclose(pipe);
                            // trim trailing newline
                            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                                result.pop_back();
                            return result;
                        };

                        json repo;
                        repo["path"] = repo_root;
                        repo["name"] = std::filesystem::path(repo_root).filename().string();

                        // Use safe.directory=* to avoid ownership checks when running as root under systemd
                        std::string git_prefix = "git -c safe.directory='*' -C \"" + repo_root + "\" ";

                        repo["branch"] = run_cmd(git_prefix + "rev-parse --abbrev-ref HEAD 2>/dev/null");

                        // Uncommitted changes count
                        std::string changes_out = run_cmd(git_prefix + "status --porcelain 2>/dev/null | wc -l");
                        repo["changes"] = std::atoi(changes_out.c_str());

                        // Ahead/behind upstream
                        std::string ahead_s = run_cmd(git_prefix + "rev-list --count @{u}..HEAD 2>/dev/null");
                        std::string behind_s = run_cmd(git_prefix + "rev-list --count HEAD..@{u} 2>/dev/null");
                        repo["ahead"] = ahead_s.empty() ? -1 : std::atoi(ahead_s.c_str());
                        repo["behind"] = behind_s.empty() ? -1 : std::atoi(behind_s.c_str());

                        arr.push_back(repo);
                    }
                    return arr.dump();
                }

                // Port-forward / IP state report (human-readable text)
                if (url_path == "/_api/wsl-forwarded-ports-list") {
                    fs::path script = fs::path(WebDashCore::Get().GetWebDashRootDirectory())
                                  / "src" / "bin" / "_webdash" / "port-forward" / "wsl-port-forward.py";

                    json out;
                    if (!fs::exists(script)) {
                        out["text"] = "Error: port-forward script not found: " + script.string();
                        return out.dump();
                    }

                    auto run_cmd_multiline = [](const std::string& cmd) -> std::string {
                        std::string result;
                        FILE* pipe = popen(cmd.c_str(), "r");
                        if (!pipe) return "";
                        char buf[512];
                        while (fgets(buf, sizeof(buf), pipe)) result += buf;
                        pclose(pipe);
                        // trim trailing whitespace
                        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t'))
                            result.pop_back();
                        return result;
                    };

                    std::string cmd = "python3 \"" + script.string() + "\" list 2>&1";
                    std::string text = run_cmd_multiline(cmd);
                    if (text.empty()) {
                        text = "Error: failed to run port-forward status command";
                    }

                    out["text"] = text;
                    return out.dump();
                }

                // Click handler for Registered Projects:
                // - If $.code.editor is set in webdash-profile.json => open project directory
                // - Else => portal will open an editor dialog (returns action=edit)
                // GET /_api/open-project?config=<absolute-path-to-webdash.config.json>
                if (url_path.rfind("/_api/open-project", 0) == 0) {
                    json resp;

                    auto q = url_path.find('?');
                    std::string cfg;
                    if (q != std::string::npos) {
                        std::string qs = url_path.substr(q + 1);
                        size_t pos = 0;
                        while (pos < qs.size()) {
                            auto amp = qs.find('&', pos);
                            auto seg = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                            auto eq  = seg.find('=');
                            if (eq != std::string::npos) {
                                auto k = seg.substr(0, eq);
                                auto v = seg.substr(eq + 1);
                                if (k == "config") cfg = url_decode(v);
                            }
                            if (amp == std::string::npos) break;
                            pos = amp + 1;
                        }
                    }

                    if (cfg.empty()) {
                        resp["ok"] = false;
                        resp["error"] = "missing config";
                        return resp.dump();
                    }

                    auto reg = suite.GetRegisteredConfig(cfg);
                    if (!reg.has_value()) {
                        resp["ok"] = false;
                        resp["error"] = "config not registered";
                        return resp.dump();
                    }

                    fs::path cfg_path(cfg);
                    fs::path dir = cfg_path.parent_path();

                    // Prefer stored user context; if missing, infer from file ownership.
                    std::optional<uid_t> uid = reg->run_as_uid;
                    std::optional<std::string> home = reg->run_as_home;
                    if (!uid.has_value()) {
                        struct stat st;
                        if (::stat(cfg.c_str(), &st) == 0) {
                            uid = st.st_uid;
                            if (!home.has_value()) {
                                home = uid_to_home(st.st_uid);
                            }
                        }
                    }

                    auto err = open_dir_in_editor(dir, uid, home);

                    if (!err.has_value()) {
                        resp["ok"] = true;
                        resp["action"] = "opened";
                        return resp.dump();
                    }
                    if (*err == "no-editor") {
                        resp["ok"] = true;
                        resp["action"] = "edit";
                        return resp.dump();
                    }
                    // If launching the editor fails, fall back to portal editing rather than doing nothing.
                    resp["ok"] = true;
                    resp["action"] = "edit";
                    resp["warning"] = *err;
                    return resp.dump();
                }

                // Read current config contents for portal editing.
                // GET /_api/read-config?config=<absolute-path-to-webdash.config.json>
                if (url_path.rfind("/_api/read-config", 0) == 0) {
                    json resp;

                    auto q = url_path.find('?');
                    std::string cfg;
                    if (q != std::string::npos) {
                        std::string qs = url_path.substr(q + 1);
                        size_t pos = 0;
                        while (pos < qs.size()) {
                            auto amp = qs.find('&', pos);
                            auto seg = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                            auto eq  = seg.find('=');
                            if (eq != std::string::npos) {
                                auto k = seg.substr(0, eq);
                                auto v = seg.substr(eq + 1);
                                if (k == "config") cfg = url_decode(v);
                            }
                            if (amp == std::string::npos) break;
                            pos = amp + 1;
                        }
                    }

                    if (cfg.empty()) {
                        resp["ok"] = false;
                        resp["error"] = "missing config";
                        return resp.dump();
                    }

                    auto reg = suite.GetRegisteredConfig(cfg);
                    if (!reg.has_value()) {
                        resp["ok"] = false;
                        resp["error"] = "config not registered";
                        return resp.dump();
                    }

                    fs::path cfg_path(cfg);
                    fs::path root = WebDashCore::Get().GetWebDashRootDirectory();
                    if (!is_under_root_dir(cfg_path, root)) {
                        resp["ok"] = false;
                        resp["error"] = "config not under webdash root";
                        return resp.dump();
                    }

                    std::ifstream in(cfg, std::ios::in);
                    if (!in.is_open()) {
                        resp["ok"] = false;
                        resp["error"] = "failed to open config";
                        return resp.dump();
                    }
                    std::stringstream ss;
                    ss << in.rdbuf();
                    resp["ok"] = true;
                    resp["content"] = ss.str();
                    return resp.dump();
                }
                // /_api/run-task?config=<path>&task=<name>
                if (url_path.rfind("/_api/run-task", 0) == 0) {
                    // Parse query string
                    auto q = url_path.find('?');
                    std::string cfg, task;
                    if (q != std::string::npos) {
                        std::string qs = url_path.substr(q + 1);
                        // simple key=value&key=value parser
                        size_t pos = 0;
                        while (pos < qs.size()) {
                            auto amp = qs.find('&', pos);
                            auto seg = qs.substr(pos, amp == std::string::npos ? amp : amp - pos);
                            auto eq  = seg.find('=');
                            if (eq != std::string::npos) {
                                auto k = seg.substr(0, eq);
                                auto v = url_decode(seg.substr(eq + 1));
                                if (k == "config") cfg  = v;
                                if (k == "task")   task = v;
                            }
                            if (amp == std::string::npos) break;
                            pos = amp + 1;
                        }
                    }
                    json resp;
                    if (cfg.empty() || task.empty()) {
                        resp["ok"] = false;
                        resp["error"] = "missing config or task parameter";
                    } else if (suite.RunTask(cfg, task)) {
                        resp["ok"] = true;
                    } else {
                        resp["ok"] = false;
                        resp["error"] = "config not found";
                    }
                    return resp.dump();
                }

                // Returns the configured terminals list from code.editor.terminals.
                // GET /_api/editor-config
                if (url_path == "/_api/editor-config") {
                    json resp;
                    const std::string key_terminals = std::string(WebDashConst::Internal::kJsonKeyPrefix) + ".code.editor.terminals";
                    auto terminals = profile_get_string_list(key_terminals);
                    resp["terminals"] = json::array();
                    for (const auto& t : terminals) {
                        resp["terminals"].push_back(t);
                    }
                    return resp.dump();
                }

                // Launch a terminal (wsl, powershell, cmd) at a project's directory.
                // GET /_api/open-terminal?config=<path>&type=<wsl|powershell|cmd>
                if (url_path.rfind("/_api/open-terminal", 0) == 0) {
                    json resp;

                    auto q = url_path.find('?');
                    std::string cfg, type;
                    if (q != std::string::npos) {
                        std::string qs = url_path.substr(q + 1);
                        size_t pos = 0;
                        while (pos < qs.size()) {
                            auto amp = qs.find('&', pos);
                            auto seg = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                            auto eq  = seg.find('=');
                            if (eq != std::string::npos) {
                                auto k = seg.substr(0, eq);
                                auto v = url_decode(seg.substr(eq + 1));
                                if (k == "config") cfg  = v;
                                if (k == "type")   type = v;
                            }
                            if (amp == std::string::npos) break;
                            pos = amp + 1;
                        }
                    }

                    if (cfg.empty() || type.empty()) {
                        resp["ok"] = false;
                        resp["error"] = "missing config or type parameter";
                        return resp.dump();
                    }

                    auto reg = suite.GetRegisteredConfig(cfg);
                    if (!reg.has_value()) {
                        resp["ok"] = false;
                        resp["error"] = "config not registered";
                        return resp.dump();
                    }

                    fs::path dir = fs::path(cfg).parent_path();

                    std::optional<uid_t> uid = reg->run_as_uid;
                    std::optional<std::string> home = reg->run_as_home;
                    if (!uid.has_value()) {
                        struct stat st;
                        if (::stat(cfg.c_str(), &st) == 0) {
                            uid = st.st_uid;
                            if (!home.has_value()) home = uid_to_home(st.st_uid);
                        }
                    }

                    auto err = open_terminal(type, dir, uid, home);
                    if (!err.has_value()) {
                        resp["ok"] = true;
                    } else {
                        resp["ok"] = false;
                        resp["error"] = *err;
                    }
                    return resp.dump();
                }

                return std::nullopt;
            };

            file_server = std::make_unique<HttpFileServer>(static_port,
                [&suite]() { return suite.GetNetworkSharedPaths(); },
                api_handler);
            file_server->Start();
        } else {
            WebDashCore::Get().Log(WebDashType::LogType::INFO,
                "Static file server disabled (no '" +
                string(WebDashConst::Public::kProfilePortStaticFileServer) +
                "' port in profile)");
        }
    }

    //
    // Register IPC command handlers, then start the WebSocket server.
    //

    // ── REGISTER ────────────────────────────────────────────────────────
    // Adds config to the list of periodically executed files.
    // Payload can be a plain path or JSON: {"path": "...", "user": "username"}
    // Fire-and-forget (no response sent).
    websocket::dispatcher.On(IpcCommand::REGISTER,
        [&suite](const IpcMsgParser::Parsed& msg, IpcDispatcher::IpcResponder /*reply*/) {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`register` request received.");

            string path;
            std::optional<uid_t> uid;
            std::optional<gid_t> gid;
            std::optional<string> home;

            // Try to parse as JSON first
            try {
                json j = json::parse(msg.payload);
                path = j["path"].get<string>();

                if (j.contains("user")) {
                    string username = j["user"].get<string>();
                    struct passwd* pw = getpwnam(username.c_str());
                    if (pw) {
                        uid = pw->pw_uid;
                        gid = pw->pw_gid;
                        home = pw->pw_dir;
                        WebDashCore::Get().Log(WebDashType::LogType::INFO,
                            "Registering as user: " + username + " (uid=" + to_string(pw->pw_uid) + ")");
                    } else {
                        WebDashCore::Get().Log(WebDashType::LogType::ERR, "Unknown user: " + username);
                        return;
                    }
                }
            } catch (...) {
                // Not JSON, treat as plain path
                path = msg.payload;
            }

            WebDashConfig dash_config(path);

            if (!dash_config.LastLodingSucceeded()) {
                WebDashCore::Get().Log(WebDashType::LogType::ERR, "Failure on loading. Skipped.");
                return;
            }

            if (uid.has_value()) {
                suite.RegisterConfig(dash_config, uid.value(), gid.value(), home.value());
            } else {
                suite.RegisterConfig(dash_config);
            }
        });

    // ── UNREGISTER ──────────────────────────────────────────────────────
    websocket::dispatcher.On(IpcCommand::UNREGISTER,
        [](const IpcMsgParser::Parsed& /*msg*/, IpcDispatcher::IpcResponder reply) {
            reply("success");
        });

    // ── RELOAD ──────────────────────────────────────────────────────────
    // Reloads all configs. Responds with a JSON array of reloaded paths.
    websocket::dispatcher.On(IpcCommand::RELOAD,
        [&suite](const IpcMsgParser::Parsed& msg, IpcDispatcher::IpcResponder reply) {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`reload` request received.");

            if (msg.payload == "*") {
                suite.ReloadAll();
            }

            json arr = suite.GetRegisteredConfigPaths();
            reply(arr.dump());
        });

    // ── LIST_REGISTERED_CONFIGS ─────────────────────────────────────────
    websocket::dispatcher.On(IpcCommand::LIST_REGISTERED_CONFIGS,
        [&suite](const IpcMsgParser::Parsed& /*msg*/, IpcDispatcher::IpcResponder reply) {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`list-registered-configs` request received.");
            reply(suite.GetRegisteredConfigsJsonDump());
        });

    // ── PING ────────────────────────────────────────────────────────────
    websocket::dispatcher.On(IpcCommand::PING,
        [](const IpcMsgParser::Parsed& /*msg*/, IpcDispatcher::IpcResponder reply) {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`ping` request received.");
            reply("success");
        });

    // ── PROCESS_ONCE ────────────────────────────────────────────────────
    // Immediately process a specific config (run all tasks with frequency).
    websocket::dispatcher.On(IpcCommand::PROCESS_ONCE,
        [&suite](const IpcMsgParser::Parsed& msg, IpcDispatcher::IpcResponder reply) {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`process-once` request received: " + msg.payload);

            const string path = msg.payload;
            WebDashConfig dash_config(path);

            if (!dash_config.LastLodingSucceeded()) {
                WebDashCore::Get().Log(WebDashType::LogType::ERR, "process-once: Failure on loading config.");
                reply("error: failed to load config");
                return;
            }

            WebDashType::RunConfig run_config;
            run_config.run_only_with_frequency = true;

            // Look up user context from registered configs
            auto registered = suite.GetRegisteredConfig(path);
            if (registered.has_value()) {
                run_config.run_as_uid = registered->run_as_uid;
                run_config.run_as_gid = registered->run_as_gid;
                run_config.run_as_home = registered->run_as_home;
                if (run_config.run_as_uid.has_value()) {
                    WebDashCore::Get().Log(WebDashType::LogType::DEBUG,
                        "process-once: using registered user context uid=" + to_string(run_config.run_as_uid.value()));
                }
            }

            auto results = dash_config.Run("", run_config);

            json response;
            response["status"] = "success";
            response["tasks_executed"] = results.size();
            reply(response.dump());
        });

    // ── RECENT_ACTIONS ──────────────────────────────────────────────────
    // Returns the most recent N executed actions as a JSON array.
    // Payload is the max count (default 20).
    websocket::dispatcher.On(IpcCommand::RECENT_ACTIONS,
        [&suite](const IpcMsgParser::Parsed& msg, IpcDispatcher::IpcResponder reply) {
            size_t max_count = 20;
            try { max_count = std::stoul(msg.payload); } catch (...) {}

            auto actions = suite.GetRecentActions(max_count);

            json arr = json::array();
            for (const auto& a : actions) {
                json item;
                item["timestamp"]   = a.timestamp;
                item["config"]      = a.config_path;
                item["task"]        = a.task_name;
                item["return_code"] = a.return_code;
                arr.push_back(item);
            }
            reply(arr.dump());
        });

    // ── RUN_TASK ─────────────────────────────────────────────────────────
    // Runs a specific task. Payload is JSON: {"config": "...", "task": "..."}
    websocket::dispatcher.On(IpcCommand::RUN_TASK,
        [&suite](const IpcMsgParser::Parsed& msg, IpcDispatcher::IpcResponder reply) {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`run-task` IPC request received.");
            json resp;
            try {
                json payload = json::parse(msg.payload);
                string cfg  = payload.value("config", "");
                string task = payload.value("task", "");

                if (cfg.empty() || task.empty()) {
                    resp["ok"] = false;
                    resp["error"] = "missing config or task";
                } else if (suite.RunTask(cfg, task)) {
                    resp["ok"] = true;
                } else {
                    resp["ok"] = false;
                    resp["error"] = "config not found";
                }
            } catch (const std::exception& e) {
                resp["ok"] = false;
                resp["error"] = string("parse error: ") + e.what();
            }
            reply(resp.dump());
        });

    // ── WIDGET_COMMAND ─────────────────────────────────────────────────
    // Payload JSON (flexible):
    //   {
    //     "type": "widget-command",
    //     "project": "<widget-identifier>",
    //     "command": "get-all-songs",
    //     "payload": <json-payload>
    //   }
    // Also supports payload.command when command is omitted at top-level.
    websocket::dispatcher.On(IpcCommand::WIDGET_COMMAND,
        [&suite](const IpcMsgParser::Parsed& msg, IpcDispatcher::IpcResponder reply) {
            json resp;
            try {
                json request = json::parse(msg.payload);

                const std::string type = request.value("type", "");
                if (!type.empty() && type != "widget-command") {
                    resp["ok"] = false;
                    resp["error"] = "invalid type for widget-command payload";
                    reply(resp.dump());
                    return;
                }

                const std::string project = request.value("project", "");
                std::string command = request.value("command", "");

                json payload = request.contains("payload") ? request["payload"] : json();
                if (command.empty() && payload.is_object() && payload.contains("command") && payload["command"].is_string()) {
                    command = payload["command"].get<std::string>();
                    payload.erase("command");
                }

                if (project.empty() || command.empty()) {
                    resp["ok"] = false;
                    resp["error"] = "missing project or command";
                    reply(resp.dump());
                    return;
                }

                auto widget_project = find_widget_project(suite, project);
                if (!widget_project.has_value()) {
                    resp["ok"] = false;
                    resp["error"] = "unknown widget project";
                    reply(resp.dump());
                    return;
                }

                auto cmd = find_widget_command(widget_project.value(), command, "widget-command");
                if (!cmd.has_value()) {
                    resp["ok"] = false;
                    resp["error"] = "unknown widget command";
                    reply(resp.dump());
                    return;
                }

                resp = execute_widget_task(suite, widget_project.value(), cmd.value(), payload);
            } catch (const std::exception& e) {
                resp["ok"] = false;
                resp["error"] = std::string("parse error: ") + e.what();
            }

            reply(resp.dump());
        });

    // ── WSL FORWARDED PORTS (server-side list) ───────────────────────────
    // Lets the client fetch port-forward status without prompting for sudo.
    // When the server runs as root under systemd, it can read iptables rules.
    websocket::dispatcher.On(IpcCommand::WSL_FORWARDED_PORTS_LIST,
        [](const IpcMsgParser::Parsed& /*msg*/, IpcDispatcher::IpcResponder reply) {
            namespace fs = std::filesystem;

            auto run_capture = [](const std::string& cmd) -> std::string {
                std::array<char, 512> buffer{};
                std::string out;
                FILE* pipe = popen(cmd.c_str(), "r");
                if (!pipe) return "";
                while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                    out += buffer.data();
                }
                pclose(pipe);
                return out;
            };

            fs::path script = fs::path(WebDashCore::Get().GetWebDashRootDirectory())
                              / "src" / "bin" / "_webdash" / "port-forward" / "wsl-port-forward.py";

            if (!fs::exists(script)) {
                reply("Error: port-forward script not found: " + script.string());
                return;
            }

            // Use the same human-readable output as the local tool.
            // Also ensure we don't block on any interactive prompt.
            std::string cmd = "python3 \"" + script.string() + "\" list 2>&1";
            std::string out = run_capture(cmd);
            if (out.empty()) {
                reply("Error: failed to run port-forward status command");
                return;
            }

            // Trim trailing whitespace/newlines to keep response tidy.
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
                out.pop_back();
            }

            reply(out);
        });

    // ── WRITE_CONFIG ───────────────────────────────────────────────────
    // Writes a registered webdash.config.json.
    // Payload JSON: {"config":"/abs/path/webdash.config.json", "content":"..."}
    websocket::dispatcher.On(IpcCommand::WRITE_CONFIG,
        [&suite](const IpcMsgParser::Parsed& msg, IpcDispatcher::IpcResponder reply) {
            json resp;
            try {
                json payload = json::parse(msg.payload);
                std::string cfg = payload.value("config", "");
                std::string content = payload.value("content", "");

                if (cfg.empty()) {
                    resp["ok"] = false;
                    resp["error"] = "missing config";
                    reply(resp.dump());
                    return;
                }

                auto reg = suite.GetRegisteredConfig(cfg);
                if (!reg.has_value()) {
                    resp["ok"] = false;
                    resp["error"] = "config not registered";
                    reply(resp.dump());
                    return;
                }

                fs::path cfg_path(cfg);
                fs::path root = WebDashCore::Get().GetWebDashRootDirectory();
                if (!is_under_root_dir(cfg_path, root)) {
                    resp["ok"] = false;
                    resp["error"] = "config not under webdash root";
                    reply(resp.dump());
                    return;
                }

                if (cfg_path.filename() != WebDashCore::GetProjectConfigFilename()) {
                    resp["ok"] = false;
                    resp["error"] = "only webdash.config.json is writable";
                    reply(resp.dump());
                    return;
                }

                fs::path tmp = cfg_path;
                tmp += ".tmp." + std::to_string(::getpid());

                std::error_code ec;
                auto perms = fs::exists(cfg_path, ec)
                    ? fs::status(cfg_path, ec).permissions()
                    : (fs::perms::owner_read | fs::perms::owner_write);

                {
                    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
                    if (!out.is_open()) {
                        resp["ok"] = false;
                        resp["error"] = "failed to open temp file";
                        reply(resp.dump());
                        return;
                    }
                    out << content;
                }

                fs::permissions(tmp, perms, ec);
                fs::rename(tmp, cfg_path, ec);
                if (ec) {
                    fs::remove(tmp, ec);
                    resp["ok"] = false;
                    resp["error"] = std::string("rename failed: ") + ec.message();
                    reply(resp.dump());
                    return;
                }

                resp["ok"] = true;
                reply(resp.dump());
            } catch (const std::exception& e) {
                resp["ok"] = false;
                resp["error"] = std::string("parse error: ") + e.what();
                reply(resp.dump());
            }
        });

    // ── Wire up Suite change notifications to broadcast to all WebSocket clients ──
    suite.SetOnChangeCallback([&suite]() {
        // Build the same JSON payloads the portal needs
        json push;
        push["type"] = "update";

        // Configs
        push["configs"] = json::parse(suite.GetRegisteredConfigsJsonDump());

        // Recent actions
        auto actions = suite.GetRecentActions(20);
        json arr = json::array();
        for (const auto& a : actions) {
            json item;
            item["timestamp"]   = a.timestamp;
            item["config"]      = a.config_path;
            item["task"]        = a.task_name;
            item["return_code"] = a.return_code;
            arr.push_back(item);
        }
        push["actions"] = arr;

        websocket::Broadcast("push:update " + push.dump());
    });
    // ── Start the WebSocket server (blocks) ─────────────────────────────
    websocket::Listen();

    return 0;
}

int main() {

    try {
       return execute_main();
    } catch (WebDashException::General& e) {
        cout << "ERROR: WebDash client received an WebDashException::General." << endl;
        cout << "MESSAGE: " << e.what() << endl;
        e.PrintBacktrace(cout);
        return 1;
    } catch (std::exception& e) {
        cout << "ERROR: WebDash client received an std::exception." << endl;
        cout << "MESSAGE: " << e.what() << endl;
        return 1;
    } catch (...) {
        cout << "ERROR: WebDash client does not handle the thrown exception." << endl;
        throw;
    }

    return 0;
}
