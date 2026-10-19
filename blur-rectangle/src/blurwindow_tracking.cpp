#include "blurwindow.h"

#include "blurwindow_constants.h"
#include "tracker.h"

#include <QLineF>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <utility>

using namespace blurwindow;

// Autotrack: every pointer move restarts the countdown. When the pointer has settled, the object under
// the spotlight is memorized by starting a tracker on it right away (beginPendingTracking); the
// spotlight only starts following it when the countdown ends.
void BlurWindow::armAutoTrack()
{
    stopTracker(std::exchange(m_pendingTracker, nullptr));
    m_idleTimer->start();
    m_settleTimer->start();
    if (!m_dotsTimer->isActive()) {
        m_dotsTimer->start();
        update();
    }
}

void BlurWindow::cancelAutoTrack()
{
    m_settleTimer->stop();
    m_idleTimer->stop();
    m_dotsTimer->stop();
    m_dragAutoTrack = false;
    stopTracker(std::exchange(m_pendingTracker, nullptr));
    update();
}

void BlurWindow::beginPendingTracking()
{
    if ((m_followingMouse || m_dragAutoTrack) && m_autoTrack && !m_tracker && !m_pendingTracker) {
        m_pendingTracker = launchTracker();
    }
}

// A tracker started on the spotlight's current area. Its updates are ignored until registerTracking().
Tracker *BlurWindow::launchTracker()
{
    if (!m_trackerFactory || !windowHandle() || !windowHandle()->screen()) {
        return nullptr;
    }
    Tracker *tracker = m_trackerFactory();
    tracker->setParent(this);
    connect(tracker, &Tracker::targetUpdated, this, [this, tracker](const QRectF &box) {
        onTargetUpdated(tracker, box);
    });
    connect(tracker, &Tracker::lost, this, [this, tracker] { stopTracker(tracker); });
    tracker->setAutoSize(m_autoSize);
    const QRectF bounds = m_apertureShape == ApertureShape::Circle ? spotlightRect() : spotlightRectangleRect();
    tracker->start(bounds.toAlignedRect(), windowHandle()->screen()->name());
    return tracker;
}

// The spotlight starts following `tracker`.
void BlurWindow::registerTracking(Tracker *tracker)
{
    m_followingMouse = false;
    m_dragAutoTrack = false;
    const QRectF bounds = m_apertureShape == ApertureShape::Circle ? spotlightRect() : spotlightRectangleRect();
    m_trackStates.insert(tracker, {QPointF(m_spotlightCenter), QPointF(m_spotlightCenter), QPointF(),
                                   bounds.width(), m_spotlightRadius, m_spotlightRectSize});
    if (!m_trackTimer->isActive()) {
        m_trackClock.start();
        m_trackTimer->start();
    }
}

void BlurWindow::startTracking()
{
    if (m_tracker) {
        return;
    }
    cancelAutoTrack();
    m_tracker = launchTracker();
    if (m_tracker) {
        registerTracking(m_tracker);
    }
}

void BlurWindow::stopTracking()
{
    stopTracker(m_tracker);
}

void BlurWindow::stopTracker(Tracker *tracker)
{
    if (!tracker) {
        return;
    }
    if (m_tracker == tracker) {
        m_tracker = nullptr;
    }
    if (m_pendingTracker == tracker) {
        m_pendingTracker = nullptr;
    }
    for (Spotlight &spotlight : m_otherSpotlights) {
        if (spotlight.tracker == tracker) {
            spotlight.tracker = nullptr;
        }
    }
    m_trackStates.remove(tracker);
    if (m_trackStates.isEmpty()) {
        m_trackTimer->stop();
    }
    tracker->stop();
    tracker->disconnect(this);
    tracker->deleteLater();
}

void BlurWindow::stopAllTracking()
{
    stopTracking();
    for (const Spotlight &spotlight : QVector<Spotlight>(m_otherSpotlights)) {
        stopTracker(spotlight.tracker);
    }
}

void BlurWindow::onTargetUpdated(Tracker *tracker, const QRectF &box)
{
    auto state = m_trackStates.find(tracker);
    if (state == m_trackStates.end()) {
        return;
    }
    // Ignore tracker jitter below the dead zone.
    if (QLineF(state->target, box.center()).length() >= kTrackDeadZone) {
        state->target = box.center();
    }
    if (!m_autoSize || state->baseWidth <= 0) {
        return;
    }
    // Follow the object's size within 20% of what the user set, never beyond the screen.
    const double ratio = qBound(1.0 - kAutoSizeRange, box.width() / state->baseWidth, 1.0 + kAutoSizeRange);
    const int radius = qBound(kMinimumSpotlightRadius, qRound(state->baseRadius * ratio),
                              qMin(kMaximumSpotlightRadius, qMin(width(), height()) / 2));
    const QSize rectSize = (state->baseSize * ratio)
                           .expandedTo(QSize(kMinimumSpotlightRectWidth, kMinimumSpotlightRectHeight))
                           .boundedTo(size());
    int *currentRadius = &m_spotlightRadius;
    QSize *currentSize = &m_spotlightRectSize;
    if (tracker != m_tracker) {
        currentRadius = nullptr;
        for (Spotlight &spotlight : m_otherSpotlights) {
            if (spotlight.tracker == tracker) {
                currentRadius = &spotlight.radius;
                currentSize = &spotlight.size;
            }
        }
        if (!currentRadius) {
            return;
        }
    }
    if (*currentRadius == radius && *currentSize == rectSize) {
        return;
    }
    *currentRadius = radius;
    *currentSize = rectSize;
    applyBlur();
    updateInputShape();
    update();
}

void BlurWindow::setAutoSize(bool enabled)
{
    m_autoSize = enabled;
    for (auto state = m_trackStates.cbegin(); state != m_trackStates.cend(); ++state) {
        state.key()->setAutoSize(enabled);
    }
    saveSettings();
}

// Critically damped spring toward the target (position and velocity both stay
// continuous). Runs at display rate, so motion stays smooth although frames
// arrive from the tracker only a few times a second.
static void smoothDamp(QPointF &position, QPointF &velocity, const QPointF &target, double smoothTime, double dt)
{
    const double omega = 2.0 / smoothTime;
    const double x = omega * dt;
    const double decay = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
    const QPointF change = position - target;
    const QPointF temp = (velocity + change * omega) * dt;
    velocity = (velocity - temp * omega) * decay;
    position = target + (change + temp) * decay;
}

void BlurWindow::stepTracking()
{
    const double dt = qBound(0.001, m_trackClock.restart() / 1000.0, 0.05);
    bool moved = false;
    for (auto state = m_trackStates.begin(); state != m_trackStates.end(); ++state) {
        smoothDamp(state->position, state->velocity, state->target, kTrackSmoothTime, dt);
        const QPoint position = state->position.toPoint();
        if (state.key() == m_tracker) {
            const QPoint clamped = clampedCenter(position, m_spotlightRadius, m_spotlightRectSize);
            moved |= clamped != m_spotlightCenter;
            m_spotlightCenter = clamped;
            continue;
        }
        for (Spotlight &spotlight : m_otherSpotlights) {
            if (spotlight.tracker == state.key()) {
                const QPoint clamped = clampedCenter(position, spotlight.radius, spotlight.size);
                moved |= clamped != spotlight.center;
                spotlight.center = clamped;
            }
        }
    }
    if (moved) {
        applyBlur();
        updateInputShape();
        update();
    }
}
