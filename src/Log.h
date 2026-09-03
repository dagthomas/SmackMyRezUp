#pragma once
#include <windows.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include "AppIdentity.h"

// Process-wide text log: one line per LOG(), mirrored to OutputDebugString.
// The file is opened (truncated) in the working directory on the first write.
// Each entry point picks its own name first thing (Log::SetFileName) so the
// player and the exporter never fight over the same file.
class Log {
public:
    static Log& Get() { static Log l; return l; }

    static void SetFileName(const char* name) {
        Log& l = Get();
        std::lock_guard<std::mutex> lock(l.m_mutex);
        if (!l.m_file.is_open()) l.m_fileName = name;
    }

    void Write(const std::string& s) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_file.is_open()) m_file.open(m_fileName, std::ios::out | std::ios::trunc);
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream line;
        line << '[' << std::setfill('0') << std::setw(2) << st.wHour << ':'
             << std::setw(2) << st.wMinute << ':' << std::setw(2) << st.wSecond
             << '.' << std::setw(3) << st.wMilliseconds << "] " << s << "\n";
        OutputDebugStringA(line.str().c_str());
        m_file << line.str();
        m_file.flush();
    }
private:
    Log() = default;
    std::string m_fileName = smru::kPlayerLogFile;
    std::ofstream m_file;
    std::mutex m_mutex;
};

#define LOG(x) do { std::ostringstream _oss; _oss << x; Log::Get().Write(_oss.str()); } while(0)
