
#include <functional>
#include <iostream>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

using namespace std;

typedef websocketpp::server<websocketpp::config::asio> server;

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

// pull out the type of messages sent by our config
typedef server::message_ptr message_ptr;

namespace websocket {
    namespace {
        bool is_closed = false;
        server echo_server;
    }

    inline std::function<void(string, string, std::function<void(string)>)> callback;

    // Requirements:
    //    For request/response actions, need to respond with same command of
    //    form <cmd>:<id>. Therefore, command is actually a 'super-command'
    //    because it contains another the actual command and cb id.
    inline void on_message(server* s, websocketpp::connection_hdl hdl, message_ptr msg) {
        (void) s;

        std::cout << "on_message called with hdl: " << hdl.lock().get()
            << " and message: " << msg->get_payload()
            << std::endl;
        WebDashCore::Get().Log(WebDashType::LogType::INFO, "Message Payload: " + msg->get_payload());

        string payload = msg->get_payload();
        std::size_t space_index = payload.find(" ");

        if (space_index == std::string::npos) {
            WebDashCore::Get().Log(WebDashType::LogType::ERR, "Message Payload malformed: Could not extract command");
            return;
        }

        string cmd = payload.substr(0, space_index);
        WebDashCore::Get().Log(WebDashType::LogType::INFO, "Command: '" + cmd + "'");

        while (space_index < payload.size() && payload[space_index] == ' ') space_index++;

        string cmd_payload = payload.substr(space_index);
        WebDashCore::Get().Log(WebDashType::LogType::INFO, "Command Payload: '" + cmd_payload + "'");

        callback(cmd, cmd_payload, [&](string resp) {
            s->send(hdl, resp, websocketpp::frame::opcode::text);
        });
    }

    inline void on_open(server* s, websocketpp::connection_hdl hdl) {
        WebDashCore::Get().Log(WebDashType::LogType::INFO, "websocket::on_open.");

        (void) s;
        (void) hdl;
    }

    inline void Stop() {
        echo_server.stop_listening();
        //echo_server.stop_perpetual();  // just in case

        int it = 5;
        while (is_closed == false && (it-- >= 0)) {
            cout << "Wait 200ms" << endl;
            usleep(100000);
        }

        if (!is_closed) {
            cout << "Forceful websocket closure." << endl;
            echo_server.stop(); // bad!
        }
    }

    inline void Listen(function<void(string, string, std::function<void(string)>)> callback) {
        WebDashCore::Get().Log(WebDashType::LogType::INFO, "Attempting to listen to: '9154'");

        websocket::callback = callback;

        try {
            // Set logging settings
            echo_server.set_access_channels(websocketpp::log::alevel::all);
            echo_server.clear_access_channels(websocketpp::log::alevel::frame_payload);
            echo_server.set_reuse_addr(true);

            // Initialize Asio
            echo_server.init_asio();

            // Register our message handler
            echo_server.set_message_handler(bind(&on_message,&echo_server,::_1,::_2));
            echo_server.set_open_handler(bind(&on_open,&echo_server,::_1));

            // Listen on port 9154
            echo_server.listen(9154);
            cout << "S1" << endl;

            // Start the server accept loop
            echo_server.start_accept();
            cout << "S2" << endl;

            // Start the ASIO io_service run loop
            echo_server.run();
            cout << "S3" << endl;

            is_closed = true;
        } catch (websocketpp::exception const & e) {
            std::cout << e.what() << std::endl;
        } catch (...) {
            std::cout << "other exception" << std::endl;
        }
    }
}

