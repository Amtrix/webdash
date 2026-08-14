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
#include <atomic>
#include <thread>
#include <map>
#include <set>

#include <ifaddrs.h>
#include <net/if.h>

#include <webdash-core.hpp>
#include <webdash-types.hpp>

using namespace std;
namespace fs = std::filesystem;

namespace {

static bool IsWslEnvironment() {
    // Heuristic: WSL kernels typically include "microsoft" in version/osrelease.
    // Keep this dependency-free and best-effort.
    auto file_contains_microsoft = [](const char* path) -> bool {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s.find("microsoft") != std::string::npos || s.find("wsl") != std::string::npos;
    };

    return file_contains_microsoft("/proc/version") || file_contains_microsoft("/proc/sys/kernel/osrelease");
}

static std::vector<std::string> GetNonLoopbackIPv4Addrs() {
    std::set<std::string> uniq;
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || !ifaddr) {
        return {};
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        char buf[INET_ADDRSTRLEN] = {0};
        auto* in = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (!inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf))) continue;

        std::string ip(buf);
        if (!ip.empty()) uniq.insert(ip);
    }

    freeifaddrs(ifaddr);
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

}  // namespace


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
        "Static file server listening on 0.0.0.0:" + to_string(_port) + " (all interfaces)");
    WebDashCore::Get().Log(WebDashType::LogType::INFO,
        "Portal URL (local): http://127.0.0.1:" + to_string(_port));

    const auto ips = GetNonLoopbackIPv4Addrs();
    if (!ips.empty()) {
        WebDashCore::Get().Log(WebDashType::LogType::INFO,
            "Portal URL (LAN/host): http://" + ips.front() + ":" + to_string(_port));
        if (ips.size() > 1) {
            WebDashCore::Get().Log(WebDashType::LogType::DEBUG,
                "Static file server: " + to_string(ips.size() - 1) + " additional interface address(es) detected");
        }
    }

    if (IsWslEnvironment()) {
        WebDashCore::Get().Log(WebDashType::LogType::INFO,
            "WSL note: If the portal isn't reachable from Windows/host, open the port: webdash port-forward " + to_string(_port));
    } else {
        WebDashCore::Get().Log(WebDashType::LogType::DEBUG,
            "Firewall note: If the portal isn't reachable remotely, ensure TCP port " + to_string(_port) + " is allowed (ufw/firewalld/nftables)."
        );
    }

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
            bool took_ownership = HandleClient(client_fd);
            if (!took_ownership) {
                close(client_fd);
            }
        }).detach();
    }
}


bool HttpFileServer::HandleClient(int client_fd) {
    // Read the HTTP request (we only need the first line)
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';

    string request(buf);

    // ── WebSocket upgrade: proxy to internal websocketpp server ──────
    {
        // Case-insensitive check for "Upgrade: websocket"
        string lower_req = request;
        transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);
        if (lower_req.find("upgrade: websocket") != string::npos) {
            ProxyWebSocket(client_fd, request);
            return true;  // ProxyWebSocket takes ownership of client_fd
        }
    }

    // Parse the request line: "GET /path HTTP/1.x"
    if (request.substr(0, 3) != "GET" && request.substr(0, 4) != "HEAD") {
        // Only support GET and HEAD
        string response = "HTTP/1.1 405 Method Not Allowed\r\n"
                         "Content-Length: 0\r\n"
                         "\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        return false;
    }

    bool is_head = (request.substr(0, 4) == "HEAD");

    // Extract the URL path
    size_t path_start = request.find(' ') + 1;
    size_t path_end = request.find(' ', path_start);
    if (path_start == string::npos || path_end == string::npos) return false;

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
        return false;
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
            return false;
        }
    }

    // Resolve the URL path to a local file
    string filepath;
    bool needs_slash_redirect = false;
    if (!ResolvePath(url_path, filepath, needs_slash_redirect)) {
        if (needs_slash_redirect) {
            string location = url_path + "/";
            string response = "HTTP/1.1 301 Moved Permanently\r\n" + cors_headers +
                             "Location: " + location + "\r\n"
                             "Content-Length: 0\r\n"
                             "\r\n";
            send(client_fd, response.c_str(), response.size(), 0);
            return false;
        }

        string body = "404 Not Found";
        string response = "HTTP/1.1 404 Not Found\r\n" + cors_headers +
                         "Content-Type: text/plain\r\n"
                         "Content-Length: " + to_string(body.size()) + "\r\n"
                         "\r\n";
        if (!is_head) response += body;
        send(client_fd, response.c_str(), response.size(), 0);
        return false;
    }

    // Open file for streaming
    ifstream file(filepath, ios::binary);
    if (!file.is_open()) {
        string body = "403 Forbidden";
        string response = "HTTP/1.1 403 Forbidden\r\n" + cors_headers +
                         "Content-Type: text/plain\r\n"
                         "Content-Length: " + to_string(body.size()) + "\r\n"
                         "\r\n";
        if (!is_head) response += body;
        send(client_fd, response.c_str(), response.size(), 0);
        return false;
    }

    string mime = GetMimeType(filepath);

    std::error_code ec;
    uint64_t file_size = fs::file_size(filepath, ec);
    if (ec) {
        string body = "500 Internal Server Error";
        string response = "HTTP/1.1 500 Internal Server Error\r\n" + cors_headers +
                         "Content-Type: text/plain\r\n"
                         "Content-Length: " + to_string(body.size()) + "\r\n"
                         "\r\n";
        if (!is_head) response += body;
        send(client_fd, response.c_str(), response.size(), 0);
        return false;
    }

    // Parse optional Range header for byte-range requests (important for media seeking).
    bool has_range = false;
    bool range_valid = false;
    uint64_t range_start = 0;
    uint64_t range_end = (file_size == 0 ? 0 : file_size - 1);

    {
        string lower_req = request;
        transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);

        size_t pos = lower_req.find("\nrange:");
        if (pos == string::npos && lower_req.rfind("range:", 0) == 0) {
            pos = 0;
        }

        if (pos != string::npos) {
            has_range = true;
            size_t line_start = (pos == 0) ? 0 : pos + 1;
            size_t line_end = request.find('\n', line_start);
            string line = request.substr(line_start, line_end == string::npos ? string::npos : line_end - line_start);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            size_t colon = line.find(':');
            string value = (colon == string::npos) ? "" : line.substr(colon + 1);
            while (!value.empty() && isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());

            const string prefix = "bytes=";
            if (value.rfind(prefix, 0) == 0 && file_size > 0) {
                string spec = value.substr(prefix.size());
                size_t dash = spec.find('-');
                if (dash != string::npos) {
                    string sstart = spec.substr(0, dash);
                    string send = spec.substr(dash + 1);

                    try {
                        if (sstart.empty()) {
                            // Suffix form: bytes=-N
                            uint64_t suffix_len = static_cast<uint64_t>(stoull(send));
                            if (suffix_len > 0) {
                                range_start = (suffix_len >= file_size) ? 0 : (file_size - suffix_len);
                                range_end = file_size - 1;
                                range_valid = true;
                            }
                        } else {
                            range_start = static_cast<uint64_t>(stoull(sstart));
                            if (!send.empty()) {
                                range_end = static_cast<uint64_t>(stoull(send));
                            } else {
                                range_end = file_size - 1;
                            }

                            if (range_start < file_size) {
                                if (range_end >= file_size) range_end = file_size - 1;
                                if (range_end >= range_start) {
                                    range_valid = true;
                                }
                            }
                        }
                    } catch (...) {
                        range_valid = false;
                    }
                }
            }
        }
    }

    if (has_range && !range_valid) {
        string response = "HTTP/1.1 416 Range Not Satisfiable\r\n" + cors_headers +
                         "Content-Type: " + mime + "\r\n"
                         "Accept-Ranges: bytes\r\n"
                         "Content-Range: bytes */" + to_string(file_size) + "\r\n"
                         "Content-Length: 0\r\n"
                         "Cache-Control: no-cache\r\n"
                         "\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        return false;
    }

    const bool is_partial = has_range && range_valid;
    const uint64_t body_start = is_partial ? range_start : 0;
    const uint64_t body_end = is_partial ? range_end : (file_size == 0 ? 0 : file_size - 1);
    const uint64_t body_len = is_partial ? (body_end - body_start + 1) : file_size;

    string response;
    if (is_partial) {
        response = "HTTP/1.1 206 Partial Content\r\n" + cors_headers +
                  "Content-Type: " + mime + "\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Range: bytes " + to_string(body_start) + "-" + to_string(body_end) + "/" + to_string(file_size) + "\r\n"
                  "Content-Length: " + to_string(body_len) + "\r\n"
                  "Cache-Control: no-cache\r\n"
                  "\r\n";
    } else {
        response = "HTTP/1.1 200 OK\r\n" + cors_headers +
                  "Content-Type: " + mime + "\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Length: " + to_string(body_len) + "\r\n"
                  "Cache-Control: no-cache\r\n"
                  "\r\n";
    }

    send(client_fd, response.c_str(), response.size(), 0);

    if (!is_head && body_len > 0) {
        file.clear();
        file.seekg(static_cast<std::streamoff>(body_start), ios::beg);

        uint64_t remaining = body_len;
        char chunk[64 * 1024];
        while (remaining > 0 && file.good()) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(chunk)));
            file.read(chunk, static_cast<std::streamsize>(want));
            std::streamsize got = file.gcount();
            if (got <= 0) break;

            size_t sent = 0;
            while (sent < static_cast<size_t>(got)) {
                ssize_t s = send(client_fd, chunk + sent, static_cast<size_t>(got) - sent, 0);
                if (s <= 0) {
                    remaining = 0;
                    break;
                }
                sent += static_cast<size_t>(s);
            }

            if (remaining < static_cast<uint64_t>(got)) break;
            remaining -= static_cast<uint64_t>(got);
        }
    }

    return false;
}


void HttpFileServer::ProxyWebSocket(int client_fd, const std::string& original_request) {
    // Connect to the internal websocketpp server on localhost:9002
    int ws_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ws_fd < 0) {
        WebDashCore::Get().Log(WebDashType::LogType::ERR,
            "WS proxy: failed to create socket: " + string(strerror(errno)));
        close(client_fd);
        return;
    }

    struct sockaddr_in ws_addr{};
    ws_addr.sin_family = AF_INET;
    ws_addr.sin_port = htons(9002);
    ws_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(ws_fd, (struct sockaddr*)&ws_addr, sizeof(ws_addr)) < 0) {
        WebDashCore::Get().Log(WebDashType::LogType::ERR,
            "WS proxy: failed to connect to localhost:9002: " + string(strerror(errno)));
        close(ws_fd);
        close(client_fd);
        return;
    }

    // Forward the original HTTP upgrade request to websocketpp
    {
        size_t sent = 0;
        while (sent < original_request.size()) {
            ssize_t s = ::send(ws_fd, original_request.c_str() + sent,
                               original_request.size() - sent, 0);
            if (s <= 0) {
                close(ws_fd);
                close(client_fd);
                return;
            }
            sent += s;
        }
    }

    // Bidirectional relay using poll()
    // We run two threads: client→ws and ws→client.
    // When either side closes, we shut down both.
    std::atomic<bool> alive{true};

    auto relay = [&alive](int from_fd, int to_fd) {
        char buf[8192];
        while (alive.load(std::memory_order_relaxed)) {
            ssize_t n = recv(from_fd, buf, sizeof(buf), 0);
            if (n <= 0) { alive.store(false); break; }
            size_t sent = 0;
            while (sent < (size_t)n) {
                ssize_t s = ::send(to_fd, buf + sent, n - sent, MSG_NOSIGNAL);
                if (s <= 0) { alive.store(false); return; }
                sent += s;
            }
        }
    };

    std::thread upstream(relay, client_fd, ws_fd);
    relay(ws_fd, client_fd);  // downstream in current thread

    alive.store(false);
    // Shut down both sockets to unblock the other thread
    shutdown(client_fd, SHUT_RDWR);
    shutdown(ws_fd, SHUT_RDWR);

    upstream.join();

    close(ws_fd);
    close(client_fd);
}


bool HttpFileServer::ResolvePath(const string& url_path, string& out_filepath, bool& needs_slash_redirect) {
    needs_slash_redirect = false;

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
                            // A directory's index.html defines relative links (href="styles.css",
                            // <script src="app.js">, etc.) against the *directory's* URL. If the
                            // requester's URL doesn't end in '/', the browser would instead resolve
                            // those links against the parent directory - breaking every relative
                            // asset. Ask the caller to redirect instead of serving the file here.
                            if (url_path.back() != '/') {
                                needs_slash_redirect = true;
                                return false;
                            }
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
