#pragma once

#include "debug.h"
#include <boost/asio.hpp>
#include <functional>

using utils::print;

namespace sn_apps {

constexpr auto TICK_FREQUENCY = std::chrono::seconds(10);

using CleanupAction = std::function<void()>;

class CleanupTimer
{

  private:
    std::thread timer_thread_;
    boost::asio::io_service service_;
    boost::asio::steady_timer timer_;

    CleanupAction action_;

    void mem_tick(const boost::system::error_code& ec)
    {

        if (ec.value() != 0) {
            print("cleanup timer error on tick : {}", ec.message());
            return;
        }

        /// todo: check if I need to catch any exceptions
        action_();

        timer_.expires_at(timer_.expiry() + TICK_FREQUENCY);
        timer_.async_wait(std::bind(&CleanupTimer::mem_tick, this, std::placeholders::_1));
    }

  public:
    CleanupTimer(CleanupAction action)
      : timer_(service_, TICK_FREQUENCY)
      , action_(action)
    {}

    ~CleanupTimer()
    {
        service_.stop();

        if (timer_thread_.joinable()) {
            timer_thread_.join();
        }
    }

    void start()
    {
        timer_.async_wait(std::bind(&CleanupTimer::mem_tick, this, std::placeholders::_1));

        timer_thread_ = std::thread([&]() { service_.run(); });
    }
};

}