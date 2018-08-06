#pragma once

#include <string>

struct MDB_env;

namespace sn_apps
{

class MessageDB
{

    using MDB_dbi = unsigned int;

  private:
    MDB_dbi m_dbi;
    MDB_env *m_env;

    void open(const std::string &filename);

  public:
    MessageDB(const std::string &db_path);

    bool save_msg(const std::string &key, const std::string &msg);

    bool retrieve_msg(const std::string &key, std::string &msg_out);
};

} // namespace lokimessenger