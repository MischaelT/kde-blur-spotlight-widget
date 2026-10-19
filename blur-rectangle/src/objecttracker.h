#pragma once

#include "tracker.h"

#include <atomic>
#include <thread>

// Follows a box with OpenCV's CSRT tracker on a worker thread, fed by the shared screen
// capture of FrameSource. Remembers what the user chose (see matcher.h) so it can find it again.
class ObjectTracker : public Tracker
{
    Q_OBJECT

public:
    explicit ObjectTracker(QObject *parent = nullptr);
    ~ObjectTracker() override;

    void start(const QRect &initialBox, const QString &screenName) override;
    void stop() override;
    void setAutoSize(bool enabled) override { m_autoSize = enabled; }

private:
    void run(QRect initialBox, QString screenName);

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_autoSize{false};
};
