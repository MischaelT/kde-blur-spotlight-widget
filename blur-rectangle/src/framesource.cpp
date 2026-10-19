#include "framesource.h"

#include <opencv2/imgproc.hpp>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDebug>
#include <QImage>
#include <QVariantMap>

#include <algorithm>
#include <map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace
{
constexpr int kMaxFrameWidth = 1920;
constexpr auto kFrameInterval = std::chrono::milliseconds(33);
constexpr int kMaxGrabFailures = 20; // consecutive failed grabs before giving up

// One full-screen frame, scaled to at most kMaxFrameWidth. `scale` is
// frame pixels per logical screen pixel.
bool grabScreen(QDBusConnection &bus, const QString &screenName, cv::Mat &frame, double &scale)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return false;
    }
    std::vector<uchar> data;
    std::thread reader([&data, readFd = fds[0]] {
        uchar buffer[1 << 16];
        ssize_t n;
        while ((n = read(readFd, buffer, sizeof(buffer))) != 0) {
            if (n < 0) {
                break;
            }
            data.insert(data.end(), buffer, buffer + n);
        }
    });

    QDBusMessage reply;
    {
        QDBusMessage call = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/ScreenShot2"),
            QStringLiteral("org.kde.KWin.ScreenShot2"), QStringLiteral("CaptureScreen"));
        call << screenName << QVariantMap() << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]));
        close(fds[1]);
        reply = bus.call(call, QDBus::Block, 5000);
    }
    reader.join();
    close(fds[0]);

    const QDBusReply<QVariantMap> result(reply);
    if (!result.isValid()) {
        qWarning() << "ScreenShot2 failed:" << reply.errorMessage();
        return false;
    }
    const QVariantMap info = result.value();
    const int width = info.value(QStringLiteral("width")).toInt();
    const int height = info.value(QStringLiteral("height")).toInt();
    const int stride = info.value(QStringLiteral("stride")).toInt();
    const auto format = static_cast<QImage::Format>(info.value(QStringLiteral("format")).toInt());
    if (width <= 0 || height <= 0 || data.size() < size_t(stride) * height) {
        qWarning() << "ScreenShot2 returned a bad image";
        return false;
    }

    const QImage bgr = QImage(data.data(), width, height, stride, format).convertToFormat(QImage::Format_BGR888);
    const cv::Mat full(height, width, CV_8UC3, const_cast<uchar *>(bgr.constBits()), bgr.bytesPerLine());
    scale = std::min(1.0, double(kMaxFrameWidth) / width);
    cv::resize(full, frame, cv::Size(), scale, scale, cv::INTER_AREA);
    return true;
}

std::mutex registryMutex;
std::map<QString, std::weak_ptr<FrameSource>> registry;
}

std::shared_ptr<FrameSource> FrameSource::acquire(const QString &screenName)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    if (auto existing = registry[screenName].lock(); existing && !existing->failed()) {
        return existing;
    }
    std::shared_ptr<FrameSource> source(new FrameSource(screenName));
    registry[screenName] = source;
    return source;
}

FrameSource::FrameSource(const QString &screenName)
    : m_screenName(screenName)
{
    m_thread = std::thread([this] { run(); });
}

FrameSource::~FrameSource()
{
    m_stop = true;
    m_thread.join();
}

std::shared_ptr<const Frame> FrameSource::next(uint64_t &lastId, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_ready.wait_for(lock, timeout, [this, lastId] {
        return m_failed || (m_latest && m_latest->id > lastId);
    });
    if (m_failed || !m_latest || m_latest->id <= lastId) {
        return nullptr;
    }
    lastId = m_latest->id;
    return m_latest;
}

void FrameSource::run()
{
    const QString connectionName = QStringLiteral("frame-source-") + m_screenName;
    {
        QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName);
        uint64_t id = 0;
        int failures = 0;
        while (!m_stop) {
            const auto started = std::chrono::steady_clock::now();
            auto frame = std::make_shared<Frame>();
            if (grabScreen(bus, m_screenName, frame->image, frame->scale)) {
                failures = 0;
                frame->id = ++id;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_latest = frame;
                }
                m_ready.notify_all();
            } else if (++failures >= kMaxGrabFailures) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_failed = true;
                }
                m_ready.notify_all();
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            std::this_thread::sleep_until(started + kFrameInterval);
        }
    }
    QDBusConnection::disconnectFromBus(connectionName);
}
