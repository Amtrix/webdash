#pragma once

/**
 * HttpFileServer - Lightweight HTTP static file server for WebDash.
 *
 * Serves files from registered project directories that have "network-shared": true
 * in their webdash.config.json. Each project is accessible under its directory name:
 *
 *   http://127.0.0.1:<port>/<project-name>/path/to/file
 *   http://<host-ip>:<port>/<project-name>/path/to/file
 *
 * Also exposes a small REST API under /_api/ for the portal dashboard:
 *
 *   GET /_api/list-configs       → JSON array of registered configs
 *   GET /_api/recent-actions     → JSON array of recent server actions
 *
 * This enables Wallpaper Engine (or any browser) to load project files via HTTP
 * instead of relying on filesystem paths (which break on WSL UNC paths).
 */

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <utility>
#include <optional>

/**
 * Callback type for API routes.
 * Takes the URL path (e.g. "/_api/list-configs") and returns a JSON body string,
 * or nullopt if the path is not a recognized API route.
 */
using ApiHandler = std::function<std::optional<std::string>(const std::string& url_path)>;

class HttpFileServer {
public:
    /**
     * @brief Construct the HTTP file server.
     * @param port The port to listen on.
     * @param path_provider A function that returns the current list of
     *        (project_name, directory_path) pairs to serve.
     * @param api_handler Optional handler for /_api/ routes.
     */
    HttpFileServer(int port,
                   std::function<std::vector<std::pair<std::string, std::string>>()> path_provider,
                   ApiHandler api_handler = nullptr);

    ~HttpFileServer();

    /**
     * @brief Start the HTTP server in a background thread.
     */
    void Start();

    /**
     * @brief Stop the HTTP server.
     */
    void Stop();

private:
    void Run();

    /**
     * @brief Handle a single HTTP client request.
     * @returns true if the function took ownership of client_fd (e.g. WebSocket proxy),
     *          in which case the caller must NOT close the fd.
     */
    bool HandleClient(int client_fd);

    /**
     * @brief Resolve a URL path to a local file path.
     * @param url_path The URL path (e.g., "/wallpaper-engine-dashboard/config/config.json").
     * @param out_filepath Output: the resolved local file path.
     * @returns true if the path resolved to a valid, accessible file.
     */
    bool ResolvePath(const std::string& url_path, std::string& out_filepath);

    /**
     * @brief Get the MIME type for a file extension.
     */
    static std::string GetMimeType(const std::string& filepath);

    /**
     * @brief URL-decode a percent-encoded string.
     */
    static std::string UrlDecode(const std::string& str);

    /**
     * @brief Proxy a WebSocket upgrade request to the internal websocketpp server.
     * Forwards the original HTTP upgrade request, then relays data bidirectionally.
     * @param client_fd The incoming client socket.
     * @param original_request The full HTTP request that was already read.
     */
    void ProxyWebSocket(int client_fd, const std::string& original_request);

    int _port;
    int _server_fd = -1;
    std::atomic<bool> _running{false};
    std::thread _server_thread;
    std::function<std::vector<std::pair<std::string, std::string>>()> _path_provider;
    ApiHandler _api_handler;
};
