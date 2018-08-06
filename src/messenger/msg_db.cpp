
#include "msg_db.h"
#include <exception>
#include <lmdb.h>
#include <boost/filesystem.hpp>
#include "misc_log_ex.h"

namespace sn_apps
{

template <typename T>
inline void throw0(const T &e)
{
    LOG_PRINT_L0(e.what());
    throw e;
}

class DB_EXCEPTION : public std::exception
{
  private:
    std::string m;

  protected:
    DB_EXCEPTION(const char *s) : m(s) {}

  public:
    virtual ~DB_EXCEPTION() {}

    const char *what() const throw()
    {
        return m.c_str();
    }
};

class DB_ERROR : public DB_EXCEPTION
{
  public:
    DB_ERROR() : DB_EXCEPTION("Generic DB Error") {}
    DB_ERROR(const char *s) : DB_EXCEPTION(s) {}
};

class DB_OPEN_FAILURE : public DB_EXCEPTION
{
  public:
    DB_OPEN_FAILURE() : DB_EXCEPTION("Failed to open the db") {}
    DB_OPEN_FAILURE(const char *s) : DB_EXCEPTION(s) {}
};

const std::string lmdb_error(const std::string &error_string, int mdb_res)
{
    const std::string full_string = error_string + mdb_strerror(mdb_res);
    return full_string;
}

MessageDB::MessageDB(const std::string &db_path)
{

    printf("MessageDB()\n");
    const auto path = boost::filesystem::path(db_path) / "messenger";

    try
    {
        open(path.string());
    }
    catch (const DB_ERROR &e)
    {
        LOG_ERROR("Error opening database: " << e.what());
    }
}

void MessageDB::open(const std::string &filename)
{

    int rc;

    boost::filesystem::path dir(filename);

    if (boost::filesystem::exists(dir))
    {
        if (!boost::filesystem::is_directory(dir))
            throw0(DB_OPEN_FAILURE("LMDB needs a directory path, but a file was passed"));
    }
    else
    {
        if (!boost::filesystem::create_directories(dir))
            throw0(DB_OPEN_FAILURE(std::string("Failed to create directory ").append(filename).c_str()));
    }

    /// Setting up LMDB environment

    if ((rc = mdb_env_create(&m_env)))
        throw0(DB_ERROR(lmdb_error("Failed to create lmdb environment: ", rc).c_str()));

    if (const auto rc = mdb_env_set_maxdbs(m_env, 1))
        throw0(DB_ERROR(lmdb_error("Failed to set max number of dbs: ", rc).c_str()));

    {
        unsigned int mdb_flags = 0;
        mdb_mode_t mode = 0644;
        if (auto rc = mdb_env_open(m_env, filename.c_str(), mdb_flags, mode))
            throw0(DB_ERROR(lmdb_error("Failed to open lmdb environment: ", rc).c_str()));
    }

    MDB_txn *txn;
    const char *db_name = "messages";

    {
        MDB_txn *parent_txn = NULL;
        unsigned int txn_flags = 0;
        if (auto rc = mdb_txn_begin(m_env, parent_txn, txn_flags, &txn))
        {
            throw0(DB_ERROR(lmdb_error("Failed to begin a transaction: ", rc).c_str()));
        }
    }

    {
        unsigned int open_flags = MDB_CREATE | MDB_DUPSORT;
        if (auto rc = mdb_dbi_open(txn, db_name, open_flags, &m_dbi))
        {
            throw0(DB_ERROR(lmdb_error("Failed to open lmdb environment: ", rc).c_str()));
        }
    }

    printf("DB successfully opened at: %s\n", filename.c_str());

    mdb_txn_commit(txn);
}

/// Encapsulates the transaction so we don't forget to close it
class read_txn_guard {

    MDB_txn* m_txn;

    public:

    ~read_txn_guard() {
        printf("abort a read transaction\n");
        mdb_txn_abort(m_txn);
    }

    int begin(MDB_env* env) {
        return mdb_txn_begin(env, NULL, MDB_RDONLY, &m_txn);
    }

    operator MDB_txn*()
    {
        return m_txn;
    }

    operator MDB_txn**()
    {
        return &m_txn;
    }


};

bool MessageDB::save_msg(const std::string &key, const std::string &msg)
{

    MDB_txn *txn;

    if (int rc = mdb_txn_begin(m_env, NULL, 0, &txn))
    {
        LOG_PRINT_L0(lmdb_error("Failed to begin a db transaction: ", rc).c_str());
        return false;
    }

    {
        MDB_val mdb_key;
        MDB_val data;

        mdb_key.mv_size = key.size();
        mdb_key.mv_data = const_cast<char *>(key.c_str());

        data.mv_size = msg.size();
        data.mv_data = const_cast<char *>(msg.c_str());

        if (int rc = mdb_put(txn, m_dbi, &mdb_key, &data, 0))
        {

            if (rc == MDB_KEYEXIST)
                printf("entry already exists\n");

            LOG_PRINT_L0(lmdb_error("Failed to put a key/value pair: ", rc).c_str());
            mdb_txn_abort(txn);
            return false;
        }
    }

    if (int rc = mdb_txn_commit(txn))
    {
        LOG_PRINT_L0(lmdb_error("Failed to commit a transaction: ", rc).c_str());
        return false;
    }
    
    return true;
}

/// Returns true on success
static bool retrieve_one(MDB_env *env, MDB_dbi dbi, MDB_val key, MDB_val &val) noexcept
{
    read_txn_guard txn;
    int rc = txn.begin(env);

    if (rc)
    {
        LOG_PRINT_L0(lmdb_error("Failed to begin a db transaction: ", rc).c_str());
        return false;
    }

    rc = mdb_get(txn, dbi, &key, &val);

    if (rc && rc != MDB_NOTFOUND)
    {
        LOG_PRINT_L0(lmdb_error("Failed to retrieve a value from db: ", rc).c_str());
        return false;
    }

    if (rc == MDB_NOTFOUND)
    {
        printf("The key is not in the database\n");
        return false;
    }

    return true;
}

static bool retrieve_all(MDB_env* env, MDB_dbi dbi, MDB_val key) noexcept
{

    read_txn_guard txn;
    int rc = txn.begin(env);
    if (rc) {
        LOG_PRINT_L0(lmdb_error("Failed to begin a db transaction: ", rc).c_str());
    }

    MDB_cursor* cur;
    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc) {
        LOG_PRINT_L0(lmdb_error("Failed to open a db cursor: ", rc).c_str());
    }

    MDB_val data;
    while ((rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT)) == 0) {
        auto msg = std::string(reinterpret_cast<char *>(data.mv_data), data.mv_size);
        printf("value: %s\n", msg.c_str());
    }

    mdb_cursor_close(cur);

    return true;
}

bool MessageDB::retrieve_msg(const std::string &key, std::string &msg_out)
{

    MDB_val mdb_key;
    MDB_val mdb_data;

    mdb_key.mv_size = key.size();
    mdb_key.mv_data = const_cast<char *>(key.c_str());

    bool res = retrieve_one(m_env, m_dbi, mdb_key, mdb_data);
    retrieve_all(m_env, m_dbi, mdb_key);

    if (res)
    {
        msg_out = std::string(reinterpret_cast<char *>(mdb_data.mv_data), mdb_data.mv_size);
        return true;
    }


    return false;
}

} // namespace lokimessenger