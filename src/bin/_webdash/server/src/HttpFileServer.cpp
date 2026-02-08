#include "HttpFileServer.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <algorithm>
#include <map>

#include <webdash-core.hpp>
#include <webdash-types.hpp>

using namespace std;
namespace fs = std::filesystem;


HttpFileServer::HttpFileServer(int port,
                               std::function<vector<pair<string, string>>()> path_provider,
                               ApiHandler api_handler)
    : _port(port), _path_provider(std::move(path_provider)), _api_handler(std::move(api_handler))
{
}


HttpFileServer::~HttpFileServer() {
    Stop();
}


void HttpFileServer::Start() {
    if (_running) return;
    _running = true;
    _server_thread = std::thread(&HttpFileServer::Run, this);
    WebDashCore::Get().Log(WebDashType::LogType::INFO,
        "Static file server starting on port " + to_string(_port));
}


void HttpFileServer::Stop() {
    _running = false;
    if (_server_fd >= 0) {
        shutdown(_server_fd, SHUT_RDWR);
        close(_server_fd);
        _server_fd = -1;
    }
    if (_server_thread.joinable()) {
        _server_thread.join();
    }
}


void HttpFileServer::Run() {
    _server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_server_fd < 0) {
        WebDashCore::Get().Log(WebDashType::LogType::ERR,
            "Static file server: Failed to create socket: " + string(strerror(errno)));
        return;
    }

    // Allow port reuse
    int opt = 1;
    setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // Accept connections on all interfaces
    addr.sin_port = htons(_port);

    if (bind(_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        WebDashCore::Get().Log(WebDashType::LogType::ERR,
            "Static file server: Failed to bind port " + to_string(_port) + ": " + string(strerror(errno)));
        close(_server_fd);
        _server_fd = -1;
        return;
    }

    if (listen(_server_fd, 16) < 0) {
        WebDashCore::Get().Log(WebDashType::LogType::ERR,
            "Static file server: Failed to listen: " + string(strerror(errno)));
        close(_server_fd);
        _server_fd = -1;
        return;
    }

    WebDashCore::Get().Log(WebDashType::LogType::INFO,
        "Static file server listening on http://localhost:" + to_string(_port));

    while (_running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(_server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (_running) {
                // Only log if we're still supposed to be running
                WebDashCore::Get().Log(WebDashType::LogType::DEBUG,
                    "Static file server: accept() failed: " + string(strerror(errno)));
            }
            continue;
        }

        // Handle each request in its own thread
        std::thread([this, client_fd]() {
            HandleClient(client_fd);
            close(client_fd);
        }).detach();
    }
}


void HttpFileServer::HandleClient(int client_fd) {
    // Read the HTTP request (we only need the first line)
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    string request(buf);

    // Parse the request line: "GET /path HTTP/1.x"
    if (request.substr(0, 3) != "GET" && request.substr(0, 4) != "HEAD") {
        // Only support GET and HEAD
        string response = "HTTP/1.1 405 Method Not Allowed\r\n"
                         "Content-Length: 0\r\n"
                         "\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        return;
    }

    bool is_head = (request.substr(0, 4) == "HEAD");

    // Extract the URL path
    size_t path_start = request.find(' ') + 1;
    size_t path_end = request.find(' ', path_start);
    if (path_start == string::npos || path_end == string::npos) return;

    string url_path = UrlDecode(request.substr(path_start, path_end - path_start));

    // Strip query string for file resolution (but keep full URL for API routing)
    string full_url = url_path;
    size_t query_pos = url_path.find('?');
    if (query_pos != string::npos) {
        url_path = url_path.substr(0, query_pos);
    }

    // CORS headers (needed for Wallpaper Engine's Chromium)
    string cors_headers = "Access-Control-Allow-Origin: *\r\n"
                         "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n";

    // Handle OPTIONS preflight
    if (request.substr(0, 7) == "OPTIONS") {
        string response = "HTTP/1.1 204 No Content\r\n" + cors_headers +
                         "Access-Control-Allow-Headers: Content-Type\r\n"
                         "Content-Length: 0\r\n"
                         "\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        return;
    }

    // ── API routes (/_api/...) ──────────────────────────────────────────
    if (_api_handler && url_path.rfind("/_api/", 0) == 0) {
        auto api_result = _api_handler(full_url);
        if (api_result.has_value()) {
            const string& body = *api_result;
            string response = "HTTP/1.1 200 OK\r\n" + cors_headers +
                             "Content-Type: application/json\r\n"
                             "Content-Length: " + to_string(body.size()) + "\r\n"
                             "Cache-Control: no-cache, no-store\r\n"
                             "\r\n";
            send(client_fd, response.c_str(), response.size(), 0);
            if (!is_head) {
                size_t sent = 0;
                while (sent < body.size()) {
                    ssize_t s = send(client_fd, body.c_str() + sent, body.size() - sent, 0);
                    if (s <= 0) break;
                    sent += s;
                }
            }
            return;
        }
    }

    // Resolve the URL path to a local file
    string filepath;
    if (!ResolvePath(url_path, filepath)) {
        string body = "404 Not Found";
        string response = "HTTP/1.1 404 Not Found\r\n" + cors_headers +
                         "Content-Type: text/plain\r\n"
                         "Content-Length: " + to_string(body.size()) + "\r\n"
                         "\r\n";
        if (!is_head) response += body;
        send(client_fd, response.c_str(), response.size(), 0);
        return;
    }

    // Read and serve the file
    ifstream file(filepath, ios::binary);
    if (!file.is_open()) {
        string body = "403 Forbidden";
        string response = "HTTP/1.1 403 Forbidden\r\n" + cors_headers +
                         "Content-Type: text/plain\r\n"
                         "Content-Length: " + to_string(body.size()) + "\r\n"
                         "\r\n";
        if (!is_head) response += body;
        send(client_fd, response.c_str(), response.size(), 0);
        return;
    }

    // Read file content
    ostringstream file_content;
    file_content << file.rdbuf();
    string body = file_content.str();

    string mime = GetMimeType(filepath);

    string response = "HTTP/1.1 200 OK\r\n" + cors_headers +
                     "Content-Type: " + mime + "\r\n"
                     "Content-Length: " + to_string(body.size()) + "\r\n"
                     "Cache-Control: no-cache\r\n"
                     "\r\n";

    send(client_fd, response.c_str(), response.size(), 0);
    if (!is_head) {
        // Send body in chunks to handle large files
        size_t sent = 0;
        while (sent < body.size()) {
            ssize_t s = send(client_fd, body.c_str() + sent, body.size() - sent, 0);
            if (s <= 0) break;
            sent += s;
        }
    }
}


bool HttpFileServer::ResolvePath(const string& url_path, string& out_filepath) {
    // URL must start with /
    if (url_path.empty() || url_path[0] != '/') return false;

    // Split into /<project_name>/rest/of/path
    string trimmed = url_path.substr(1);  // remove leading /
    size_t slash_pos = trimmed.find('/');

    string project_name;
    string rel_path;

    if (slash_pos == string::npos) {
        // URL is just /project — treat as directory root
        project_name = trimmed;
        rel_path = "";
    } else {
        project_name = trimmed.substr(0, slash_pos);
        rel_path = trimmed.substr(slash_pos + 1);
    }

    if (project_name.empty()) return false;

    // Security: reject path traversal
    if (rel_path.find("..") != string::npos) return false;

    // Look up the project in the current shared paths
    auto shared_paths = _path_provider();
    for (const auto& [name, dir] : shared_paths) {
        if (name == project_name) {
            fs::path full_path = fs::path(dir) / rel_path;

            // Resolve to canonical and verify it's still under the project dir
            try {
                fs::path canonical = fs::canonical(full_path);
                fs::path canonical_dir = fs::canonical(dir);

                string canonical_str = canonical.string();
                string dir_str = canonical_dir.string();

                if (canonical_str.find(dir_str) != 0) {
                    return false;  // Path escaped the project directory
                }

                if (!fs::is_regular_file(canonical)) {
                    // If it's a directory, try index.html
                    if (fs::is_directory(canonical)) {
                        fs::path index_path = canonical / "index.html";
                        if (fs::is_regular_file(index_path)) {
                            out_filepath = index_path.string();
                            return true;
                        }
                    }
                    return false;  // Not a file
                }

                out_filepath = canonical_str;
                return true;
            } catch (...) {
                return false;  // File doesn't exist or other error
            }
        }
    }

    return false;
}


string HttpFileServer::GetMimeType(const string& filepath) {
    static const map<string, string> mime_types = {
        {".html",  "text/html"},
        {".htm",   "text/html"},
        {".css",   "text/css"},
        {".js",    "application/javascript"},
        {".json",  "application/json"},
        {".txt",   "text/plain"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".svg",   "image/svg+xml"},
        {".ico",   "image/x-icon"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf",   "font/ttf"},
        {".mp3",   "audio/mpeg"},
        {".mp4",   "video/mp4"},
        {".webm",  "video/webm"},
        {".webp",  "image/webp"},
        {".xml",   "application/xml"},
        {".pdf",   "application/pdf"},
    };

    fs::path p(filepath);
    string ext = p.extension().string();

    // Convert to lowercase
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto it = mime_types.find(ext);
    if (it != mime_types.end()) return it->second;
    return "application/octet-stream";
}


string HttpFileServer::UrlDecode(const string& str) {
    string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int val = 0;
            istringstream hex_stream(str.substr(i + 1, 2));
            if (hex_stream >> hex >> val) {
                result += static_cast<char>(val);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }

    return result;
}
