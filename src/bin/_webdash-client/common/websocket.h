#pragma once

// Self
#include "utils.hpp"

// Standard
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <sstream>
#include <string>
#include <optional>

// External
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/common/memory.hpp>

using namespace std;


typedef websocketpp::client<websocketpp::config::asio_client> client;


class connection_metadata
{
public:
    typedef websocketpp::lib::shared_ptr<connection_metadata> ptr;

    connection_metadata(int id, websocketpp::connection_hdl hdl, std::string uri)
      : m_id(id)
      , m_hdl(hdl)
      , m_status("Connecting")
      , m_uri(uri)
      , m_server("N/A")
    {}

    void on_open(client * c, websocketpp::connection_hdl hdl)
    {
        m_status = "Open";

        client::connection_ptr con = c->get_con_from_hdl(hdl);
        m_server = con->get_response_header("Server");
    }

    void on_fail(client * c, websocketpp::connection_hdl hdl)
    {
        m_status = "Failed connecting";

        client::connection_ptr con = c->get_con_from_hdl(hdl);
        m_server = con->get_response_header("Server");
        m_error_reason = con->get_ec().message();
    }

    void on_close(client * c, websocketpp::connection_hdl hdl)
    {
        m_status = "Closed";
        client::connection_ptr con = c->get_con_from_hdl(hdl);
        std::stringstream s;
        s << "close code: " << con->get_remote_close_code() << " ("
          << websocketpp::close::status::get_string(con->get_remote_close_code())
          << "), close reason: " << con->get_remote_close_reason();
        m_error_reason = s.str();
    }

    void on_message(websocketpp::connection_hdl, client::message_ptr msg)
    {
        WebDashCore::Get().Log(WebDashType::LogType::INFO, "Received message from server: '" + msg->get_payload() + "'");

        if (msg->get_opcode() == websocketpp::frame::opcode::text)
        {
            m_response.push_back(msg->get_payload());
        }
        else
        {
            m_response.push_back(websocketpp::utility::to_hex(msg->get_payload()));
        }
    }

    websocketpp::connection_hdl get_hdl() const
    {
        return m_hdl;
    }

    int get_id() const
    {
        return m_id;
    }

    std::string get_status() const
    {
        return m_status;
    }

    void record_sent_message(std::string message)
    {
        m_messages.push_back(">> " + message);
    }

    std::optional<string> get_response(string prefix)
    {
        for (size_t i = 0; i < m_response.size(); ++i)
            if (m_response[i].length() > prefix.length() && m_response[i].substr(0, prefix.length()) == prefix)
                return m_response[i].substr(prefix.length() + 1);

        return {};
    }

    friend std::ostream & operator<< (std::ostream & out, connection_metadata const & data);

private:

    int m_id;
    websocketpp::connection_hdl m_hdl;
    std::string m_status;
    std::string m_uri;
    std::string m_server;
    std::string m_error_reason;
    std::vector<std::string> m_messages;
    std::vector<std::string> m_response;
};


std::ostream & operator<< (std::ostream & out, connection_metadata const & data)
{
    out << "> URI: " << data.m_uri << "\n"
        << "> Status: " << data.m_status << "\n"
        << "> Remote Server: " << (data.m_server.empty() ? "None Specified" : data.m_server) << "\n"
        << "> Error/close reason: " << (data.m_error_reason.empty() ? "N/A" : data.m_error_reason) << "\n";
    out << "> Messages Processed: (" << data.m_messages.size() << ") \n";

    std::vector<std::string>::const_iterator it;
    for (it = data.m_messages.begin(); it != data.m_messages.end(); ++it)
    {
        out << *it << "\n";
    }

    return out;
}


class websocket_endpoint
{
public:
    websocket_endpoint () : m_next_id(0)
    {
        m_endpoint.clear_access_channels(websocketpp::log::alevel::all);
        m_endpoint.clear_error_channels(websocketpp::log::elevel::all);

        m_endpoint.init_asio();
        m_endpoint.start_perpetual();

        m_thread = websocketpp::lib::make_shared<websocketpp::lib::thread>(&client::run, &m_endpoint);
    }

    ~websocket_endpoint()
    {
        m_endpoint.stop_perpetual();

        for (con_list::const_iterator it = m_connection_list.begin(); it != m_connection_list.end(); ++it)
        {
            if (it->second->get_status() != "Open")
            {
                continue;
            }

            WebDashCore::Get().Log(WebDashType::LogType::INFO, "> Closing connection " + to_string(it->second->get_id()));

            websocketpp::lib::error_code ec;
            m_endpoint.close(it->second->get_hdl(), websocketpp::close::status::going_away, "", ec);

            if (ec) WebDashCore::Get().Log(WebDashType::LogType::ERR, "Error closing connection " + to_string(it->second->get_id()) + " " + ec.message());
        }

        m_thread->join();
    }

    int connect(std::string const & uri)
    {
        websocketpp::lib::error_code ec;

        client::connection_ptr con = m_endpoint.get_connection(uri, ec);

        if (ec) throw std::runtime_error("Connect initialization error: " + ec.message());

        int new_id = m_next_id++;
        connection_metadata::ptr metadata_ptr = websocketpp::lib::make_shared<connection_metadata>(new_id, con->get_handle(), uri);
        m_connection_list[new_id] = metadata_ptr;

        con->set_open_handler(websocketpp::lib::bind(
            &connection_metadata::on_open,
            metadata_ptr,
            &m_endpoint,
            websocketpp::lib::placeholders::_1
        ));
        con->set_fail_handler(websocketpp::lib::bind(
            &connection_metadata::on_fail,
            metadata_ptr,
            &m_endpoint,
            websocketpp::lib::placeholders::_1
        ));
        con->set_close_handler(websocketpp::lib::bind(
            &connection_metadata::on_close,
            metadata_ptr,
            &m_endpoint,
            websocketpp::lib::placeholders::_1
        ));
        con->set_message_handler(websocketpp::lib::bind(
            &connection_metadata::on_message,
            metadata_ptr,
            websocketpp::lib::placeholders::_1,
            websocketpp::lib::placeholders::_2
        ));

        m_endpoint.connect(con);

        return new_id;
    }

    void close(int id,
               websocketpp::close::status::value code,
               std::string reason)
    {
        websocketpp::lib::error_code ec;

        con_list::iterator metadata_it = m_connection_list.find(id);

        if (metadata_it == m_connection_list.end())
            throw std::runtime_error("No connection found with id " + to_string(id));

        m_endpoint.close(metadata_it->second->get_hdl(), code, reason, ec);

        if (ec) throw std::runtime_error("Error initiating close: " + ec.message());
    }

    void send(int id, std::string message)
    {
        websocketpp::lib::error_code ec;
        con_list::iterator metadata_it = m_connection_list.find(id);

        if (metadata_it == m_connection_list.end())
            throw std::runtime_error("Connection with given ID " + to_string(id) + " not found.");

        m_endpoint.send(metadata_it->second->get_hdl(), message, websocketpp::frame::opcode::text, ec);

        if (ec) throw std::runtime_error("Sending message '" + ec.message()  + "' failed.");

        WebDashCore::Get().Log(WebDashType::LogType::INFO, "Sent message to server: '" + message + "'");
    }


    /*
     * Sends a message to the WebDash server and waits for a response.
     *
     * Returns:
     *     bool - TRUE on SUCCESS.
     */
    void sendAndWaitForResponse(int id, std::string cmd, std::string msg, std::function<void(string)> cb)
    {
        con_list::iterator metadata_it = m_connection_list.find(id);

        if (metadata_it == m_connection_list.end())
            throw std::runtime_error("Connection with given ID not found.");

        //
        // Construct the message. Format:
        //
        //     <command>:<callback identifier> <message>.
        //
        // Send it.
        //

        string prefix = cmd + ":" + AddLeftPadding(5, '0', to_string(m_message_to_resp_id++));
        send(id, prefix + " " + msg);

        //
        // Look for response next 1 second.
        //

        for (size_t i = 0; i < 5; ++i)
        {
            auto ret = metadata_it->second->get_response(prefix);

            if (ret.has_value())
            {
                cb(*ret);
                return;
            }

            usleep(1000 * 1000); // 1 milliseconds * 100
        }

        throw std::runtime_error("Websocket timeout.");
    }

    connection_metadata::ptr get_metadata(int id) const
    {
        con_list::const_iterator metadata_it = m_connection_list.find(id);

        if (metadata_it == m_connection_list.end())
        {
            return connection_metadata::ptr();
        }
        else
        {
            return metadata_it->second;
        }
    }

private:

    typedef std::map<int,connection_metadata::ptr> con_list;

    client m_endpoint;
    websocketpp::lib::shared_ptr<websocketpp::lib::thread> m_thread;

    con_list m_connection_list;
    int m_next_id;
    int m_message_to_resp_id;
};


int ConnectEndpoint(websocket_endpoint *endpoint)
{
    int id = endpoint->connect("ws://localhost:9154");

    if (id == -1) throw std::runtime_error("Error creating connection.");

    for (int i = 0; i < 10; ++i)
    {
        if (endpoint->get_metadata(id)->get_status() == "Open") break;
        usleep(1000 * 100); // 1 milliseconds * 100
    }

    return id;
}


void WebDashRegister(string configPath)
{
    websocket_endpoint endpoint;

    int id = ConnectEndpoint(&endpoint);
    endpoint.send(id, "register " + configPath);
}


void WebDashUnRegister(string configPath)
{
    websocket_endpoint endpoint;

    int id = ConnectEndpoint(&endpoint);

    endpoint.sendAndWaitForResponse(
        id,
        "unregister",
        configPath,
        [&](__attribute__((unused)) string resp){
            cout << "Success: Config was removed from automatic management." << endl;
    });
}


/*
 * Send a ping to the WebDash server. This function can only throw upon failure,
 * it should not happen.
 */
void WebDashPingServer()
{
    websocket_endpoint endpoint;

    int id = ConnectEndpoint(&endpoint);

    endpoint.sendAndWaitForResponse(id,
                                    "ping",
                                    "",
                                    [&](__attribute__((unused)) string resp)
    {
        cout << "Success: Server is running." << endl;
    });
}


void WebDashReloadAll()
{
    websocket_endpoint endpoint;
    int id = ConnectEndpoint(&endpoint);
    endpoint.send(id, "reload *");
}


vector<string> WebDashList(string configPath)
{
    WebDashConfig wdConfig(configPath);
    auto cmds = wdConfig.GetTaskList();

    vector<string> ret;
    for (size_t i = 0; i < cmds.size(); ++i)
    {
        ret.push_back(cmds[i]);
    }

    return ret;
}


void WebDashConfigList()
{
    websocket_endpoint endpoint;

    int id = ConnectEndpoint(&endpoint);

    endpoint.sendAndWaitForResponse(id, "list-config", "*", [&](string resp)
    {
        auto parsed_resp = json::parse(resp);

        for (string elem : parsed_resp)
            cout << "   " << elem << endl;
    });
}
