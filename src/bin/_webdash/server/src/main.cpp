#include <iostream>
#include <signal.h>
#include <fstream>
#include <pwd.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <webdash-config.hpp>
#include <webdash-config-task.hpp>
#include <websocket.h>

#include "Suite.hpp"
#include "WebDashCore.hpp"

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
    // On the thread that keeps on going, we register a listening callback.
    //

    websocket::Listen([&] (std::string cmd,
                           std::string payload,
                           std::function<void(string)> callback) {

        //
        // Adds config to the list of periodically executed files.
        // Special properties of each command then indicate if it is to be
        // executed. Currently, this includes { "frequency": "daily" }.
        // Payload can be just a path, or JSON: {"path": "...", "user": "username"}
        //

        if (cmd == "register") {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`register` request received.");

            string path;
            std::optional<uid_t> uid;
            std::optional<gid_t> gid;
            std::optional<string> home;

            // Try to parse as JSON first
            try {
                json j = json::parse(payload);
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
                path = payload;
            }

            WebDashConfig dash_config(path);

            if (dash_config.LastLodingSucceeded() == false) {
                WebDashCore::Get().Log(WebDashType::LogType::ERR, "Failure on loading. Skipped.");
                return;
            }

            if (uid.has_value()) {
                suite.RegisterConfig(dash_config, uid.value(), gid.value(), home.value());
            } else {
                suite.RegisterConfig(dash_config);
            }
        }

        //
        // Removes a config from the list of periodically executed files.
        //

        if (cmd == "unregister") {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`unregister` request received.");

            const string path = payload;

            suite.UnRegisterConfig(path);
        }

        //
        // Reloads all configs. Useful to get any changes take effect.
        //

        if (cmd.substr(0, 6) == "reload" && payload == "*") {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`reload *` request received.");
            suite.ReloadAll();

            // If there's a callback ID, respond with list of reloaded configs
            if (cmd.length() > 7 && cmd[6] == ':') {
                json arr = suite.GetRegisteredConfigPaths();
                callback(cmd + " " + arr.dump());
            }
        }

        //
        // All commands that follow own a callback routine on the client's side.
        // This is why we differentiate the commands based on the prefix:
        // Each command is of form "<command>:<callbackid>" and when responding,
        // this exact same string (including the callback id) has to be
        // responded to. The parameters to the callback are to be provided
        // following an extra space
        //

        if (cmd.substr(0, 23) == "list-registered-configs") {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`list-registered-configs` request received.");

            string cbid = cmd.substr(24);
            callback(cmd + " " + suite.GetRegisteredConfigsJsonDump());
        }

        if (cmd.substr(0, 4) == "ping") {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`ping` request received.");

            string cbid = cmd.substr(5);
            callback(cmd + " success");
        }

        //
        // Immediately process a specific config (run all tasks with frequency).
        // Useful for debugging scheduled execution.
        // Uses the user context if the config is registered with one.
        //

        if (cmd.substr(0, 12) == "process-once") {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`process-once` request received: " + payload);

            const string path = payload;
            WebDashConfig dash_config(path);

            if (dash_config.LastLodingSucceeded() == false) {
                WebDashCore::Get().Log(WebDashType::LogType::ERR, "process-once: Failure on loading config.");
                if (cmd.length() > 13 && cmd[12] == ':') {
                    callback(cmd + " error: failed to load config");
                }
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

            if (cmd.length() > 13 && cmd[12] == ':') {
                json response;
                response["status"] = "success";
                response["tasks_executed"] = results.size();
                callback(cmd + " " + response.dump());
            }
        }
    });

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
