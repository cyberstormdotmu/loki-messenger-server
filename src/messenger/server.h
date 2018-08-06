#pragma once

#include <cstdio>
#include "net/http_server_impl_base.h"

namespace sn_apps
{

    class MessageDB;
    class ServiceDB;

struct COMMAND_SEND_MESSAGE
{

    struct request
    {
        std::string message;
        std::string pub_key;
        int ttl; // time to live in minutes

        BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(pub_key)
        KV_SERIALIZE(message)
        KV_SERIALIZE(ttl)
        END_KV_SERIALIZE_MAP()
    };

    struct response
    {
        std::string status;

        BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(status)
        END_KV_SERIALIZE_MAP()
    };
};

struct COMMAND_GET_MESSAGE
{

    struct request {

        std::string pub_key;
        BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(pub_key)
        END_KV_SERIALIZE_MAP()

    };

    struct response {

        std::string value;
        std::string status;

        BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(value)
        KV_SERIALIZE(status)
        END_KV_SERIALIZE_MAP()
    };

};

class messenger_server : public epee::http_server_impl_base<messenger_server>
{

    using connection_context = epee::net_utils::connection_context_base;
    using http_request_info = epee::net_utils::http::http_request_info;
    using http_response_info = epee::net_utils::http::http_response_info;


  private:
    
    std::unique_ptr<MessageDB> m_message_db;

    /// Database used by a service node for temporary storage
    std::unique_ptr<ServiceDB> m_sn_db;

  public:

    messenger_server(const std::string& db_path);
    ~messenger_server();

    bool handle_http_request(const http_request_info &query_info,
                             http_response_info &response,
                             connection_context &m_conn_context) override;

    template <class t_context>
    bool handle_http_request_map(const http_request_info &query_info,
                                 http_response_info &response_info,
                                 t_context &m_conn_context);

    bool on_send_message(const COMMAND_SEND_MESSAGE::request &req, COMMAND_SEND_MESSAGE::response &res);

    bool on_get_message(const COMMAND_GET_MESSAGE::request &req, COMMAND_GET_MESSAGE::response &res);
};

} // namespace lokimessenger

/// what constitutes an http server?

///     listens on a port
///     parses a message

/// what would the API look like?
