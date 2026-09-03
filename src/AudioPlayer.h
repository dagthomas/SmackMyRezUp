#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    bool Start(const std::wstring& videoPath, double seekSeconds = 0.0);
    bool Seek(double seconds);
    void Pause(bool paused);
    void SetVolume(float volume01);
    void Stop();
    double PositionSeconds() const;

private:
    bool StartProcess(double seekSeconds);
    void StopProcess();
    void ThreadMain();

    std::wstring m_path;
    std::wstring m_ffmpeg;
    HANDLE m_process = nullptr;
    HANDLE m_stdout = nullptr;
    HWAVEOUT m_waveOut = nullptr;
    std::thread m_thread;
    mutable std::mutex m_waveMutex;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_hasAudioData{false};
    double m_seekBaseSec = 0.0;
    float m_volume = 1.0f;
};
