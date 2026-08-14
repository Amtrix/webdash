/**
 * @file webdash-ipc.hpp
 * @brief Shared IPC (Inter-Process Communication) protocol between webdash-client and webdash-server.
 *
 * This header is the single source of truth for the WebSocket-based communication
 * between the CLI client and the long-running server. Both sides include this file
 * so they always agree on command names, wire format, and message semantics.
 *
 * ## Wire format
 *
 *   Fire-and-forget:   "<command> <payload>"
 *   Request/Response:  "<command>:<callbackID> <payload>"
 *                      "<command>:<callbackID> <response>"
 *
 * callbackID is a zero-padded 5-digit integer (e.g. "00003").
 *
 * ## Adding a new command
 *
 *   1. Add an entry to the `commands` array in `IpcRegistry::All()`.
 *   2. On the server side, register a handler via `IpcDispatcher::On(IpcCommand::MY_CMD, ...)`.
 *   3. On the client side, use `IpcMsgBuilder` to construct the outgoing message and
 *      `IpcMsgParser` to interpret the response (or use the convenience RPC helpers).
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <optional>
#include <unordered_map>
#include <stdexcept>
#include <sstream>
#include <iomanip>


/**
 * Well-known IPC commands.
 * Each value corresponds to the wire prefix used in WebSocket messages.
 */
enum class IpcCommand {
    CONFIG_REGISTER,
    CONFIG_UNREGISTER,
    RELOAD,
    LIST_REGISTERED_CONFIGS,
    PING,
    PROCESS_ONCE,
    RECENT_ACTIONS,
    RUN_TASK,
    TOOLS_PORTS_FORWARDED_LIST,
    WRITE_CONFIG,
    WIDGET_COMMAND,
    OPEN_WIDGET_WINDOW,
};


/**
 * Describes a single IPC command: its enum tag, wire string, and whether it
 * expects a response (request/response) or is fire-and-forget.
 */
struct IpcCommandInfo {
    IpcCommand      id;
    std::string     wire;           ///< The string that travels on the wire (e.g. "register").
    bool            expects_reply;  ///< true → client uses sendAndWaitForResponse.
    std::string     description;    ///< Human-readable short description.
};


/**
 * Central registry of every IPC command the system understands.
 *
 * This is intentionally a plain function returning a const ref to a static
 * vector so it works from header-only inclusion in both binaries.
 */
struct IpcRegistry {

    static const std::vector<IpcCommandInfo>& All() {
        static const std::vector<IpcCommandInfo> commands = {
            // ──────────────────────────────────────────────────────────────
            // id                               wire                        reply?  description
            // ──────────────────────────────────────────────────────────────
            { IpcCommand::CONFIG_REGISTER,              "register",                 false,  "Register a config for periodic execution" },
            { IpcCommand::CONFIG_UNREGISTER,            "unregister",               true,   "Unregister a config from tracking" },
            { IpcCommand::RELOAD,                "reload",                   true,   "Reload all registered configs" },
            { IpcCommand::LIST_REGISTERED_CONFIGS,"list-registered-configs", true,   "List all registered config paths" },
            { IpcCommand::PING,                  "ping",                     true,   "Health-check the server" },
            { IpcCommand::PROCESS_ONCE,          "process-once",             true,   "Immediately run a config's scheduled tasks" },
            { IpcCommand::RECENT_ACTIONS,        "recent-actions",           true,   "Get the most recent server-executed actions" },
            { IpcCommand::RUN_TASK,               "run-task",                  true,   "Run a specific task by config path and name" },
            { IpcCommand::TOOLS_PORTS_FORWARDED_LIST,"wsl-forwarded-ports-list", true,   "List configured WSL forwarded ports (server-side, no client sudo)" },
            { IpcCommand::WRITE_CONFIG,           "write-config",             true,   "Write a registered webdash.config.jsonc (portal editor)" },
            { IpcCommand::WIDGET_COMMAND,         "widget-command",           true,   "Execute a widget-command and return its output" },
            { IpcCommand::OPEN_WIDGET_WINDOW,     "open-widget-window",       true,   "Launch a widget in a native app-style browser window" },
        };
        return commands;
    }

    /// Look up command info by enum.
    static const IpcCommandInfo& Get(IpcCommand cmd) {
        for (const auto& info : All()) {
            if (info.id == cmd) return info;
        }
        throw std::logic_error("IpcRegistry::Get: unknown IpcCommand");
    }

    /// Look up command info by wire string prefix (matches the part before ':').
    static std::optional<std::reference_wrapper<const IpcCommandInfo>> FindByWire(std::string_view wire_prefix) {
        for (const auto& info : All()) {
            if (wire_prefix == info.wire ||
                (wire_prefix.size() > info.wire.size() &&
                 wire_prefix[info.wire.size()] == ':' &&
                 wire_prefix.substr(0, info.wire.size()) == info.wire))
            {
                return std::cref(info);
            }
        }
        return std::nullopt;
    }
};


// ═══════════════════════════════════════════════════════════════════════════
//  Message building helpers (used by the client)
// ═══════════════════════════════════════════════════════════════════════════

struct IpcMsgBuilder {

    /**
     * Build a fire-and-forget message (no callback ID).
     * Wire: "<command> <payload>"
     */
    static std::string FireAndForget(IpcCommand cmd, const std::string& payload) {
        return IpcRegistry::Get(cmd).wire + " " + payload;
    }

    /**
     * Generate a callback-ID string: zero-padded to 5 digits.
     */
    static std::string MakeCallbackId(int seq) {
        std::ostringstream oss;
        oss << std::setw(5) << std::setfill('0') << seq;
        return oss.str();
    }

    /**
     * Build a request that expects a response.
     * Wire: "<command>:<callbackID> <payload>"
     * Returns the full prefix (command:cbid) as second element for response matching.
     */
    static std::pair<std::string, std::string> Request(IpcCommand cmd, int seq, const std::string& payload) {
        const auto& info = IpcRegistry::Get(cmd);
        std::string prefix = info.wire + ":" + MakeCallbackId(seq);
        std::string message = prefix + " " + payload;
        return { message, prefix };
    }
};


// ═══════════════════════════════════════════════════════════════════════════
//  Message parsing helpers (used by the server)
// ═══════════════════════════════════════════════════════════════════════════

struct IpcMsgParser {

    /// Result of parsing an incoming WebSocket message.
    struct Parsed {
        std::string                 full_cmd;       ///< Full command token (e.g. "reload:00001").
        std::string                 base_cmd;       ///< Base command (e.g. "reload").
        std::optional<std::string>  callback_id;    ///< Callback ID if present.
        std::string                 payload;        ///< Everything after the first space.

        /// The matched registry entry (if recognized command).
        std::optional<std::reference_wrapper<const IpcCommandInfo>> info;
    };

    /**
     * Parse a raw WebSocket payload into structured parts.
     * Returns nullopt if the message is completely malformed (no space separator).
     */
    static std::optional<Parsed> Parse(const std::string& raw) {
        std::size_t space = raw.find(' ');
        if (space == std::string::npos) {
            return std::nullopt;
        }

        Parsed p;
        p.full_cmd = raw.substr(0, space);

        // Skip any extra spaces
        std::size_t payload_start = space;
        while (payload_start < raw.size() && raw[payload_start] == ' ') ++payload_start;
        p.payload = raw.substr(payload_start);

        // Split full_cmd into base_cmd and optional callback_id
        std::size_t colon = p.full_cmd.find(':');
        if (colon != std::string::npos) {
            p.base_cmd = p.full_cmd.substr(0, colon);
            p.callback_id = p.full_cmd.substr(colon + 1);
        } else {
            p.base_cmd = p.full_cmd;
        }

        p.info = IpcRegistry::FindByWire(p.base_cmd);
        return p;
    }
};


// ═══════════════════════════════════════════════════════════════════════════
//  Server-side command dispatcher
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Clean command-handler dispatcher for the server.
 *
 * Usage:
 *   IpcDispatcher dispatcher;
 *   dispatcher.On(IpcCommand::PING, [](const IpcMsgParser::Parsed& msg, IpcResponder reply) {
 *       reply("success");
 *   });
 *
 *   // In the websocket on_message callback:
 *   dispatcher.Dispatch(raw_payload, send_fn);
 */
class IpcDispatcher {
public:
    /**
     * The responder function a handler calls to send a reply.
     * It automatically prepends the full_cmd prefix for callback-ID matching.
     * For fire-and-forget commands this is a no-op.
     */
    using IpcResponder = std::function<void(const std::string& response_body)>;

    /**
     * Handler signature:
     *   - parsed message (command, payload, callback ID, etc.)
     *   - responder function (call with just the body; prefix is auto-prepended)
     */
    using Handler = std::function<void(const IpcMsgParser::Parsed& msg, IpcResponder reply)>;

    /**
     * Register a handler for a specific command.
     */
    void On(IpcCommand cmd, Handler handler) {
        _handlers[cmd] = std::move(handler);
    }

    /**
     * Dispatch an incoming raw WebSocket message.
     *
     * @param raw_payload  The full message text from the wire.
     * @param raw_send     Low-level send function from the websocket layer.
     * @returns true if the command was recognized and dispatched; false otherwise.
     */
    bool Dispatch(const std::string& raw_payload,
                  std::function<void(const std::string&)> raw_send) const
    {
        auto parsed = IpcMsgParser::Parse(raw_payload);
        if (!parsed) {
            return false;  // Malformed
        }

        if (!parsed->info) {
            return false;  // Unknown command
        }

        IpcCommand cmd_id = parsed->info->get().id;
        auto it = _handlers.find(cmd_id);
        if (it == _handlers.end()) {
            return false;  // No handler registered
        }

        // Build the responder: automatically prepends "full_cmd " to the reply body.
        IpcResponder responder = [&parsed, &raw_send](const std::string& body) {
            raw_send(parsed->full_cmd + " " + body);
        };

        it->second(*parsed, responder);
        return true;
    }

private:
    std::unordered_map<IpcCommand, Handler> _handlers;
};


// ═══════════════════════════════════════════════════════════════════════════
//  IPC connection constants
// ═══════════════════════════════════════════════════════════════════════════

namespace IpcConfig {
    /// Default WebSocket port for client ↔ server communication.
    constexpr int kWebSocketPort = 9002;

    /// WebSocket URI used by the client to connect to the server.
    constexpr const char* kWebSocketUri = "ws://localhost:9002";

    /// Default timeout (seconds) for request/response calls.
    constexpr int kDefaultTimeoutSec = 5;

    /// Extended timeout for long-running operations (e.g. process-once).
    constexpr int kExtendedTimeoutSec = 10;
}
