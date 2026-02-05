#include <iostream>
#include <signal.h>
#include <fstream>

#include <nlohmann/json.hpp>

#include <webdash-config.hpp>
#include <webdash-config-task.hpp>
#include <websocket.h>

#include "Suite.hpp"
#include "WebDashCore.hpp"

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
			if (lockf(pid_fd, F_ULOCK, 0) < 0) {
                perror("lockf pid_fd failed.");
                exit(EXIT_FAILURE);
            }
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

int execute_main()
{
    cout << "Starting server..." << endl;
    /*{
        // TODO: Handle race condition here.

        if (system("pgrep -c webdash-server > /tmp/webdash_server-proc_count") != 0) {
            cout << "pgrep failed (counts process 'webdash-server'): Process already exists?" << endl;
            return 1;
        }

        ifstream in("/tmp/webdash_server-proc_count");
        int count;
        in >> count;

        if(count > 1) {
            cout << "A webdash-server process is already running." << endl;
            return 1;
        }
    }*/

    // https://github.com/jirihnidek/daemon/blob/master/src/daemon.c
    // https://www.man7.org/linux/man-pages/man3/daemon.3.html
    //daemonize();

    //cout << "daemonize() succeeded" << endl;

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

    cout << "Calling Listen X... " << endl;

    websocket::Listen([&] (std::string cmd,
                           std::string payload,
                           std::function<void(string)> callback) {

        //
        // Adds config to the list of periodically executed files.
        // Special properties of each command then indicate if it is to be
        // executed. Currently, this includes { "frequency": "daily" }.
        //

        if (cmd == "register") {
            WebDashCore::Get().Log(WebDashType::LogType::INFO, "`register` request received.");

            const string path = payload;
            WebDashConfig dash_config(path);

            if (dash_config.LastLodingSucceeded() == false) {
                WebDashCore::Get().Log(WebDashType::LogType::ERR, "Failure on loading. Skipped.");
                return;
            }

            suite.RegisterConfig(dash_config);
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
