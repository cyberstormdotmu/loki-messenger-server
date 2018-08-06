#pragma once;

#include <chrono>
#include <unordered_map>
#include "debug.h"

namespace utils {

class PerformanceHelper
{

    using HRClock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<HRClock>;

    using ms = std::chrono::milliseconds;
    using ns = std::chrono::nanoseconds;

    struct PerfInstance
    {
        long long current = 0;
        TimePoint begin;
    };

  private:
    HRClock m_hrClock;
    TimePoint m_begin;
    const char* m_message;

    std::unordered_map<const char*, PerfInstance> maps;

  public:
    void begin(const char* msg)
    {
        m_message = msg;
        m_begin = m_hrClock.now();
    }

    void accumulate(const char* msg) { maps[msg].begin = m_hrClock.now(); }

    void end(const char* msg)
    {
        auto now = m_hrClock.now();
        auto duration_ns = std::chrono::duration_cast<ns>(now - maps[msg].begin).count();
        maps[msg].current += duration_ns;
        maps[msg].begin = now;
    }

    void total(const char* msg)
    {
        auto dur = maps[msg].current;
        std::cout << "Duration(" << msg << "): " << dur / 1000000 << "ms"
                  << " (" << dur << "ns)\n";
    }

    void end()
    {
        auto duration_ms = std::chrono::duration_cast<ms>(m_hrClock.now() - m_begin).count();
        auto duration_ns = std::chrono::duration_cast<ns>(m_hrClock.now() - m_begin).count();
        print("Duration({}): {}ms ({}ns)", m_message, duration_ms, duration_ns);
    }
};

}