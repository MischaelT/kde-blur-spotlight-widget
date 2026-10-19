#pragma once

#include <QString>

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

struct Frame {
    cv::Mat image; // BGR, at most kMaxFrameWidth wide
    double scale;  // image pixels per logical screen pixel
    uint64_t id;
};

// Grabs one screen through KWin's ScreenShot2 D-Bus API on a worker thread and hands the
// latest frame to any number of trackers, so several trackers share one capture loop.
// The loop runs for as long as someone holds the source.
class FrameSource
{
public:
    // The source for `screenName`, shared with every other holder.
    static std::shared_ptr<FrameSource> acquire(const QString &screenName);
    ~FrameSource();

    // Waits up to `timeout` for a frame newer than `lastId`. Null on timeout or when capture failed.
    std::shared_ptr<const Frame> next(uint64_t &lastId, std::chrono::milliseconds timeout);
    bool failed() const { return m_failed; }

private:
    explicit FrameSource(const QString &screenName);
    void run();

    QString m_screenName;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_failed{false};
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::shared_ptr<const Frame> m_latest;
};
