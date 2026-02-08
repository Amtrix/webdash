
#include <functional>
#include <iostream>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

// Shared IPC protocol (client ↔ server agreement)
#include <webdash-ipc.hpp>

using namespace std;

typedef websocketpp::server<websocketpp::config::asio> server;

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

typedef server::message_ptr message_ptr;

namespace websocket {
    namespace {
        bool is_closed = false;
        server echo_server;
    }

    /// The dispatcher handles all incoming commands via registered handlers.
    inline IpcDispatcher dispatcher;

    /**
     * WebSocket on_message handler.
     * Delegates entirely to the IpcDispatcher which matches the command
     * and invokes the correct handler.
     */
    inline void on_message(server* s, websocketpp::connection_hdl hdl, message_ptr msg) {
        (void) s;

        const string& raw = msg->get_payload();

        auto send_fn = [s, hdl](const string& resp) {
            s->send(hdl, resp, websocketpp::frame::opcode::text);
        };

        if (!dispatcher.Dispatch(raw, send_fn)) {
            std::cerr << "Warning: unhandled IPC message: "
                      << raw.substr(0, std::min<size_t>(raw.size(), 80)) << std::endl;
        }
    }

    inline void on_open(server* s, websocketpp::connection_hdl hdl) {
        (void) s;
        (void) hdl;
    }

    inline void Stop() {
        echo_server.stop_listening();

        int it = 5;
        while (is_closed == false && (it-- >= 0)) {
            cout << "Wait 200ms" << endl;
            usleep(100000);
        }

        if (!is_closed) {
            cout << "Forceful websocket closure." << endl;
            echo_server.stop();
        }
    }

    /**
     * Start the WebSocket server and block.
     *
     * Before calling this, register all command handlers via:
     *   websocket::dispatcher.On(IpcCommand::XXX, handler);
     */
    inline void Listen() {
        try {
            echo_server.set_access_channels(websocketpp::log::alevel::all);
            echo_server.clear_access_channels(websocketpp::log::alevel::frame_payload);
            echo_server.set_reuse_addr(true);

            echo_server.init_asio();

            echo_server.set_message_handler(bind(&on_message, &echo_server, ::_1, ::_2));
            echo_server.set_open_handler(bind(&on_open, &echo_server, ::_1));

            echo_server.listen(IpcConfig::kWebSocketPort);
            echo_server.start_accept();
            echo_server.run();

            is_closed = true;
        } catch (websocketpp::exception const & e) {
            std::cout << e.what() << std::endl;
        } catch (...) {
            std::cout << "other exception" << std::endl;
        }
    }
}
