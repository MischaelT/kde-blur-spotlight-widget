#include "objecttracker.h"

#include "framesource.h"
#include "matcher.h"

#include <opencv2/tracking.hpp>

#include <QMetaObject>

#include <chrono>
#include <cmath>

namespace
{
constexpr int kMaxMissedFrames = 5;
constexpr int kRefreshFrames = 10;     // on a substitute, look for the chosen object this often
constexpr int kViewFrames = 60;        // how often to consider remembering a new look of the chosen object
constexpr double kNewViewMinScore = 0.7; // still clearly the chosen object ...
constexpr double kNewViewMaxScore = 0.9; // ... but looking different enough to be worth keeping
constexpr int kFastSearchFrames = 90;  // search every frame-few while freshly lost, then slower
constexpr int kFastSearchEvery = 3;
constexpr int kSlowSearchEvery = 10;
}

ObjectTracker::ObjectTracker(QObject *parent)
    : Tracker(parent)
{
}

ObjectTracker::~ObjectTracker()
{
    stop();
}

void ObjectTracker::start(const QRect &initialBox, const QString &screenName)
{
    stop();
    m_stop = false;
    m_thread = std::thread([this, initialBox, screenName] { run(initialBox, screenName); });
}

void ObjectTracker::stop()
{
    m_stop = true;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ObjectTracker::run(QRect initialBox, QString screenName)
{
    const std::shared_ptr<FrameSource> source = FrameSource::acquire(screenName);
    uint64_t lastId = 0;

    cv::Ptr<cv::TrackerCSRT> tracker;
    double scale = 1.0;
    int missed = 0;
    int lostFrames = 0;
    int sinceRefresh = 0;
    int sinceView = 0;
    bool searching = false;
    bool onSubstitute = false; // following a look-alike while the chosen object is missing
    bool failed = false;
    cv::Mat previous;
    matcher::TargetMemory memory; // the chosen object, never replaced by a look-alike

    auto emitBox = [this, &scale](const cv::Rect &box) {
        const QRectF screenBox(box.x / scale, box.y / scale, box.width / scale, box.height / scale);
        QMetaObject::invokeMethod(this, [this, screenBox] { emit targetUpdated(screenBox); }, Qt::QueuedConnection);
    };

    while (!m_stop) {
        const std::shared_ptr<const Frame> shot = source->next(lastId, std::chrono::milliseconds(50));
        if (!shot) {
            if (source->failed()) {
                failed = true;
                break;
            }
            continue;
        }
        const cv::Mat &frame = shot->image;
        scale = shot->scale;
        const bool bigChange = matcher::frameChanged(frame, previous);

        auto acquire = [&](const cv::Rect &box) {
            tracker = cv::TrackerCSRT::create();
            tracker->init(frame, box);
            missed = 0;
            searching = false;
            emitBox(box);
        };

        if (!tracker) {
            const cv::Rect box = cv::Rect(qRound(initialBox.x() * scale), qRound(initialBox.y() * scale),
                                          qRound(initialBox.width() * scale), qRound(initialBox.height() * scale))
                & cv::Rect(0, 0, frame.cols, frame.rows);
            if (box.width < 8 || box.height < 8) {
                failed = true;
                break;
            }
            tracker = cv::TrackerCSRT::create();
            tracker->init(frame, box);
            memory.setChosen(frame(box));
        } else if (searching) {
            // Target lost: prefer the chosen object itself, else the closest look-alike, which is
            // followed until the chosen object shows up again. Searches get rarer the longer it takes.
            ++lostFrames;
            const int every = lostFrames <= kFastSearchFrames ? kFastSearchEvery : kSlowSearchEvery;
            if (lostFrames % every != 0) {
                continue;
            }
            cv::Rect found;
            switch (matcher::afterLoss(memory.best(frame, found, m_autoSize))) {
            case matcher::Outcome::Chosen:
                onSubstitute = false;
                acquire(found);
                break;
            case matcher::Outcome::Substitute:
                onSubstitute = true;
                sinceRefresh = 0;
                acquire(found);
                break;
            case matcher::Outcome::None:
                break; // keep looking, the chosen object may come back
            }
        } else {
            cv::Rect box;
            if (tracker->update(frame, box)) {
                missed = 0;
                lostFrames = 0;
                emitBox(box);
                if (bigChange || (onSubstitute && ++sinceRefresh >= kRefreshFrames)) {
                    // The picture changed a lot, or we follow a substitute: check where the chosen
                    // object is and switch (back) to it when it is clearly there.
                    sinceRefresh = 0;
                    cv::Rect found;
                    const double best = memory.best(frame, found, m_autoSize);
                    const bool far = std::hypot(found.x - box.x, found.y - box.y) > box.width / 2.0;
                    if (best >= 0.0 && matcher::switchTarget(onSubstitute, best, far, memory.scoreAt(frame, box))) {
                        onSubstitute = false;
                        acquire(found);
                    }
                }
                if (!onSubstitute && ++sinceView >= kViewFrames) {
                    // Remember a new look of the chosen object (lighting, angle), but only while it
                    // still clearly is the chosen object, so a drifting tracker cannot teach it wrong.
                    sinceView = 0;
                    const double here = memory.scoreAt(frame, box);
                    const cv::Rect inside = box & cv::Rect(0, 0, frame.cols, frame.rows);
                    if (here >= kNewViewMinScore && here < kNewViewMaxScore && inside.width >= 8 && inside.height >= 8) {
                        memory.addView(frame(inside));
                    }
                }
            } else if (++missed >= kMaxMissedFrames) {
                searching = true;
                lostFrames = 0;
            }
        }
    }
    if (failed && !m_stop) {
        QMetaObject::invokeMethod(this, [this] { emit lost(); }, Qt::QueuedConnection);
    }
}
