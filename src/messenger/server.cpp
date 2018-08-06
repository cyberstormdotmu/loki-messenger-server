#include "server.h"
#include "debug.h"
#include "msg_db.h"
#include "sn_db.h"

#include "perf_helper.h"

using namespace epee;

using utils::print;

namespace sn_apps {

/// Explicit desctructor is necessary for std::unqiue_ptr
// messenger_server::~messenger_server() = default;

messenger_server::~messenger_server() {

    print("~messenger_server");
}

messenger_server::messenger_server(const std::string& db_path)
  : m_sn_db(new ServiceDB(db_path))
{}

template<class t_context>
bool messenger_server::handle_http_request_map(const http_request_info& query_info,
                                               http_response_info& response_info,
                                               t_context& m_conn_context)
{

    utils::PerformanceHelper perf_helper;

    perf_helper.begin("handle request");
    /// this takes just under 1ms per request...

    bool handled = false;
    if (false) return true; // just a stub to have "else if" in macros
    MAP_URI_AUTO_JON2("/send_message", on_send_message, COMMAND_SEND_MESSAGE)
    MAP_URI_AUTO_JON2("/get_message", on_get_message, COMMAND_GET_MESSAGE)

    perf_helper.end();

    return handled;
}

bool messenger_server::handle_http_request(const http_request_info& query_info,
                                           http_response_info& response,
                                           connection_context& m_conn_context)
{
    // return true; /// just a test
    LOG_PRINT_L2("HTTP [" << m_conn_context.m_remote_address.host_str() << "] " << query_info.m_http_method_str << " "
                          << query_info.m_URI);
    response.m_response_code = 200;
    response.m_response_comment = "Ok";
    if (!handle_http_request_map(query_info, response, m_conn_context)) {
        response.m_response_code = 404;
        response.m_response_comment = "Not found";
    }
    return true;
}

bool messenger_server::on_send_message(const COMMAND_SEND_MESSAGE::request& req, COMMAND_SEND_MESSAGE::response& res)
{

    
    try {
        m_sn_db->save_msg({ req.pub_key, req.message, req.ttl });
        print("saved message {} for key {}", req.message, req.pub_key);
        res.status = "saved";
        return true;
    } catch (std::exception& e) {
        print("Exception caught: {}", e.what());
        res.status = "error";
        return false;
    }
}

bool messenger_server::on_get_message(const COMMAND_GET_MESSAGE::request& req, COMMAND_GET_MESSAGE::response& res)
{

    std::vector<std::string> result;
    try {
        result = m_sn_db->retrieve_msg(req.pub_key);

        res.value = utils::format("{}", result);
        res.status = "Ok";
        // print("retrieved value for key [{}]: {}", req.pub_key, res.value);

    } catch (std::exception& e) {
        print("Exception caught: {}", e.what());
        res.status = "Error";
        return false;
    }

    return true;
}

} // namespace sn_apps
