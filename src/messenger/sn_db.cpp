#include "sn_db.h"
#include "cleanup_timer.h"
#include "debug.h"
#include "misc_log_ex.h"
#include "perf_helper.h"
#include "sqlite/sqlite3.h"

#include <boost/filesystem.hpp>
#include <chrono>
#include <fstream>
#include <thread>

using Clock = std::chrono::system_clock;
using utils::print;
using TimePoint = std::chrono::time_point<Clock>;

namespace sn_apps {

class DB_EXCEPTION : public std::exception
{
  private:
    std::string m;

  public:
    DB_EXCEPTION(const char* s)
      : m(s)
    {}
    virtual ~DB_EXCEPTION() {}

    const char* what() const throw() { return m.c_str(); }
};

static uint64_t get_time_ms()
{
    auto timestamp = Clock::now();

    uint64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count();

    return milliseconds;
}

static TimePoint time_from_ms(uint64_t ms)
{
    const auto dur = std::chrono::milliseconds(ms);
    return TimePoint(dur);
}

static void print_time_from_ms(uint64_t ms)
{

    const auto time_point = time_from_ms(ms);

    const auto tp = Clock::to_time_t(time_point);

    print("time: {}", std::ctime(&tp));
}

void CloseDB::operator()(sqlite3* db)
{
    if (sqlite3_close(db) != SQLITE_OK) {
        print("ERROR: could not close db");
    }
}

void CloseSqlStatement::operator()(sqlite3_stmt* stmt)
{
    if (sqlite3_finalize(stmt) != SQLITE_OK) {
        print("ERROR: could not finalize a statement in db");
    }
}

typedef int (*SQL_Callback)(void*, int, char**, char**);

/// Execute `sql` query; return true on success
static bool execute_query(sqlite3* db, const char* query, SQL_Callback cb = nullptr, void* arg = nullptr)
{
    char* zErrMsg = 0;

    auto rc = sqlite3_exec(db, query, cb, arg, &zErrMsg);
    bool success = (rc == SQLITE_OK);

    if (!success) {
        print(zErrMsg);
    }

    sqlite3_free(zErrMsg);
    return success;
}

/// Open at `path` or create a new database and associate it with `db`
static Sqlite3 open_db(const char* path)
{
    const bool exists = boost::filesystem::exists(path);

    if (!exists) {
        std::ofstream file(path);
        file.close();
    } else {
        MGINFO("opening an existing DB at " << path);
    }

    sqlite3* db;

    // TODO: "Whether or not an error occurs when it is opened, resources associated with the database connection handle
    // should be released by passing it to sqlite3_close() when it is no longer required."

    /// Open the database in the fully serialised mode
    int res = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);

    if (res != SQLITE_OK) {
        printf("ERROR: error opening a DB file\n");
        return nullptr;
    }

    if (!exists) {
        MGINFO("creating a new DB at " << path);

        const auto result = execute_query(db, "CREATE TABLE Data( \
            Owner varchar(256) NOT NULL, \
            TimeReceived INTEGER NOT NULL, \
            TimeExpires INTEGER NOT NULL, \
            Data BLOB \
        );");

        if (!result) return nullptr;
    }

    return Sqlite3(db);
}

/// Prepare statement using `query`
static SqlStatement prepare_statement(sqlite3* db, const char* query)
{
    const char* pzTest;

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, &pzTest);
    if (rc != SQLITE_OK) {
        print("ERROR: sql error: {}", pzTest);
    }

    return SqlStatement{ stmt };
}

/// This takes from 0ms to 30ms (depending on the size of the DB)
void ServiceDB::perform_cleanup()
{

    // print("processed (save) total: {}", msg_processed_);

    utils::PerformanceHelper perf_helper;

    perf_helper.begin("removing expired DB entries");
    // std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto now_ms = get_time_ms();
    const auto query = utils::format("DELETE FROM data WHERE TimeExpires < {};", now_ms);

    if (!execute_query(db_.get(), query.c_str())) {
        print("ERROR occurred when trying to remove expired entries");
    } else {
        print("successfully removed expired entries");
    }

    perf_helper.end();
}

ServiceDB::ServiceDB(const std::string& db_path)
{
    const auto path = boost::filesystem::path(db_path) / "service_node_db";

    try {
        open_and_prepare(path.string());

        cleanup_timer_.reset(new CleanupTimer(std::bind(&ServiceDB::perform_cleanup, this)));
        cleanup_timer_->start();

    } catch (std::exception& e) {
        print("Exception caught: {}", e.what());
    }
}

ServiceDB::~ServiceDB() = default;

void ServiceDB::open_and_prepare(const std::string& db_path)
{
    db_ = open_db(db_path.c_str());
    if (!db_) throw DB_EXCEPTION("could not create/open a database file");

    const char* save_query = "INSERT INTO Data \
                         (Owner, TimeReceived, TimeExpires, Data) \
                         VALUES (?,?,?,?);";
    save_stmt_ = prepare_statement(db_.get(), save_query);
    if (!save_stmt_) throw DB_EXCEPTION("could not prepare the save statement");

    const char* get_query = "SELECT * FROM Data WHERE Owner = ?;";
    get_stmt_ = prepare_statement(db_.get(), get_query);
    if (!get_stmt_) throw DB_EXCEPTION("could not prepare the get statement");
}

void ServiceDB::save_msg(const Message& msg)
{

    // std::this_thread::sleep_for(std::chrono::microseconds(50));
    msg_processed_++;

    // const std::string pubkey = msg.pub_key;
    // const auto cur_time = get_time_ms();
    // const auto exp_time = cur_time + msg.ttl;

    // if (sqlite3_reset(save_stmt_.get()) != SQLITE_OK) {
    //     throw DB_EXCEPTION("ERROR: could not reset DB statement");
    // }

    // sqlite3_bind_text(save_stmt_.get(), 1, pubkey.c_str(), -1, SQLITE_STATIC);
    // sqlite3_bind_int64(save_stmt_.get(), 2, cur_time);
    // sqlite3_bind_int64(save_stmt_.get(), 3, exp_time);
    // sqlite3_bind_blob(save_stmt_.get(), 4, msg.message.c_str(), -1, SQLITE_STATIC);

    // while (true) {
    //     int rc = sqlite3_step(save_stmt_.get());

    //     if (rc == SQLITE_BUSY) {
    //         print("DATABASE BUSY");
    //         continue;
    //     } else if (rc == SQLITE_DONE) {
    //         msg_processed_++;
    //         break;
    //     } else {
    //         throw DB_EXCEPTION("ERROR: could not execute db statement");
    //     }
    // }
}

std::vector<std::string> ServiceDB::retrieve_msg(const std::string& key)
{

    int rc = sqlite3_reset(get_stmt_.get());
    if (rc != SQLITE_OK) {
        throw DB_EXCEPTION("ERROR: could not reset DB statement");
    }

    sqlite3_bind_text(get_stmt_.get(), 1, key.c_str(), -1, SQLITE_STATIC);

    std::vector<std::string> results;

    while (true) {

        int res = sqlite3_step(get_stmt_.get());

        if (res == SQLITE_DONE) break;

        if (res != SQLITE_ROW) throw DB_EXCEPTION("ERROR: SQL runtime error");

        const auto pub_key = (const char*)sqlite3_column_text(get_stmt_.get(), 0);
        const auto time_saved = sqlite3_column_int64(get_stmt_.get(), 1);
        const auto time_expires = sqlite3_column_int64(get_stmt_.get(), 2);

        const auto size = sqlite3_column_bytes(get_stmt_.get(), 3);
        const char* rawdata = (const char*)sqlite3_column_blob(get_stmt_.get(), 3);

        results.emplace_back(rawdata, size);
    }

    return results;
}

} // namespace sn_apps