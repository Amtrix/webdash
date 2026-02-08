#include <iostream>
#include <signal.h>
#include <fstream>
#include <pwd.h>
#include <sys/file.h>
#include <unistd.h>

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

const string _WEBDASH_PROJECT_NAME_ = "webdash-server";

static char pid_file_name[] = "/tmp/webdash-server.lock";
static int pid_fd = -1;

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
                // /_api/run-task?config=<path>&task=<name>
                if (url_path.rfind("/_api/run-task", 0) == 0) {
                    // Parse query string
                    auto q = url_path.find('?');
                    std::string cfg, task;
                    if (q != std::string::npos) {
                        std::string qs = url_path.substr(q + 1);
                        // simple key=value&key=value parser
                        auto decode = [](const std::string& s) {
                            std::string out; out.reserve(s.size());
                            for (size_t i = 0; i < s.size(); ++i) {
                                if (s[i] == '%' && i + 2 < s.size()) {
                                    int hi = 0, lo = 0;
                                    auto hex = [](char c) -> int {
                                        if (c >= '0' && c <= '9') return c - '0';
                                        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                                        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                                        return -1;
                                    };
                                    hi = hex(s[i+1]); lo = hex(s[i+2]);
                                    if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
                                }
                                if (s[i] == '+') { out += ' '; continue; }
                                out += s[i];
                            }
                            return out;
                        };
                        size_t pos = 0;
                        while (pos < qs.size()) {
                            auto amp = qs.find('&', pos);
                            auto seg = qs.substr(pos, amp == std::string::npos ? amp : amp - pos);
                            auto eq  = seg.find('=');
                            if (eq != std::string::npos) {
                                auto k = seg.substr(0, eq);
                                auto v = decode(seg.substr(eq + 1));
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
