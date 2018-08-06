#pragma once

#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace sn_apps {

class CleanupTimer;

struct CloseDB
{
    void operator()(sqlite3* db);
};

struct CloseSqlStatement
{
    void operator()(sqlite3_stmt* stmt);
};

using Sqlite3 = std::unique_ptr<sqlite3, CloseDB>;
using SqlStatement = std::unique_ptr<sqlite3_stmt, CloseSqlStatement>;

struct Message
{

    std::string pub_key;
    std::string message;
    int ttl; /// time to live in ms
};

class ServiceDB
{

    Sqlite3 db_;

    SqlStatement save_stmt_;
    SqlStatement get_stmt_;

    std::unique_ptr<CleanupTimer> cleanup_timer_;

    int msg_processed_ = 0;

    void open_and_prepare(const std::string& db_path);

    void perform_cleanup();

  public:
    ServiceDB(const std::string& db_path);
    ~ServiceDB();

    void save_msg(const Message& msg);

    std::vector<std::string> retrieve_msg(const std::string& key);
};

} // namespace sn_apps