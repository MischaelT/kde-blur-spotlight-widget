#include "blurwindow.h"

#include "blurwindow_constants.h"
#include "objecttracker.h"

#include <KWindowEffects>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolTip>
#include <QWindow>

#include <algorithm>

using namespace blurwindow;

namespace
{
void drawTitleButton(QWidget *widget,
                     QPainter *painter,
                     const QRect &rect,
                     bool hovered,
                     bool pressed,
                     QStyle::StandardPixmap icon)
{
    QStyleOptionToolButton option;
    option.initFrom(widget);
    option.rect = rect;
    option.state |= QStyle::State_AutoRaise;
    if (hovered) {
        option.state |= QStyle::State_MouseOver;
    }
    if (pressed) {
        option.state |= QStyle::State_Sunken;
    } else {
        option.state |= QStyle::State_Raised;
    }
    option.icon = widget->style()->standardIcon(icon);
    option.iconSize = QSize(16, 16);
    option.toolButtonStyle = Qt::ToolButtonIconOnly;
    widget->style()->drawComplexControl(QStyle::CC_ToolButton, &option, painter, widget);
}

Qt::Edges toQtEdges(BlurWindow::Edges edges)
{
    Qt::Edges result;
    if (edges.testFlag(BlurWindow::LeftEdge)) {
        result |= Qt::LeftEdge;
    }
    if (edges.testFlag(BlurWindow::TopEdge)) {
        result |= Qt::TopEdge;
    }
    if (edges.testFlag(BlurWindow::RightEdge)) {
        result |= Qt::RightEdge;
    }
    if (edges.testFlag(BlurWindow::BottomEdge)) {
        result |= Qt::BottomEdge;
    }
    return result;
}
}

BlurWindow::BlurWindow(QWidget *parent)
    : QWidget(parent)
    , m_settings(QStringLiteral("local"), QStringLiteral("blur-rectangle"))
    , m_tintColor(255, 255, 255)
    , m_trackerFactory([] { return new ObjectTracker; })
{
    setWindowTitle(QStringLiteral("Blur Spotlight"));
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    loadSettings();
    createTrayIcon();
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(kAutoTrackIdleMs);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        // Spotlight stuck to the mouse and the mouse stood still for the whole countdown: follow the
        // object that was under it when the pointer settled, not what is under it now.
        m_dotsTimer->stop();
        if ((m_followingMouse || m_dragAutoTrack) && m_autoTrack) {
            if (Tracker *pending = std::exchange(m_pendingTracker, nullptr)) {
                m_tracker = pending;
                registerTracking(pending);
            } else {
                startTracking();
            }
            updateInputShape();
        }
        update();
    });
    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(kAutoTrackSettleMs);
    connect(m_settleTimer, &QTimer::timeout, this, &BlurWindow::beginPendingTracking);
    // The input mask only reaches the compositor a frame or two late, so a fast pointer can outrun
    // a following spotlight. While the pointer moves, the input area is wider; it narrows again when still.
    m_followMoveTimer = new QTimer(this);
    m_followMoveTimer->setSingleShot(true);
    m_followMoveTimer->setInterval(kFollowSettleMs);
    connect(m_followMoveTimer, &QTimer::timeout, this, [this] {
        m_followMoving = false;
        updateInputShape();
    });
    m_dotsTimer = new QTimer(this);
    m_dotsTimer->setInterval(200);
    connect(m_dotsTimer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    m_trackTimer = new QTimer(this);
    m_trackTimer->setInterval(kTrackFrameMs);
    m_trackTimer->setTimerType(Qt::PreciseTimer);
    connect(m_trackTimer, &QTimer::timeout, this, &BlurWindow::stepTracking);
}

BlurWindow::~BlurWindow()
{
    stopAllTracking();
    saveSettings();
}

void BlurWindow::setTrackerFactory(std::function<Tracker *()> factory)
{
    m_trackerFactory = std::move(factory);
}

void BlurWindow::showDefaultMode()
{
    setMode(Mode::Spotlight);
    setApertureShape(ApertureShape::Circle);
    setSpotlightInverted(true);
}

void BlurWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    event->accept();
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

void BlurWindow::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
    m_hovered = true;
    updateInputShape();
    updateCursor(event->position().toPoint());
    update();
}

void BlurWindow::hideEvent(QHideEvent *event)
{
    stopAllTracking();
    publishZoomRegions();
    saveSettings();
    QWidget::hideEvent(event);
}

void BlurWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_drawingOutline) {
            cancelOutline();
            return;
        }
        if (m_tracker || std::any_of(m_otherSpotlights.cbegin(), m_otherSpotlights.cend(),
                                     [](const Spotlight &s) { return s.tracker; })) {
            stopAllTracking();
            return;
        }
        if (m_dragAutoTrack) {
            cancelAutoTrack();
            return;
        }
        if (m_followingMouse) {
            m_followingMouse = false;
            cancelAutoTrack();
            updateInputShape();
            saveSettings();
            update();
            return;
        }
        close();
        return;
    }
    if (event->key() == Qt::Key_T && event->modifiers().testFlag(Qt::ControlModifier)) {
        setClickThrough(!m_clickThrough);
        return;
    }
    if (event->key() == Qt::Key_S && event->modifiers() == Qt::NoModifier) {
        setMode(isSpotlightMode() ? Mode::BlurRectangle : Mode::Spotlight);
        return;
    }
    if (event->key() == Qt::Key_Q && event->modifiers() == Qt::ControlModifier) {
        qApp->quit();
        return;
    }
    if (event->key() == Qt::Key_H && event->modifiers() == Qt::NoModifier) {
        hide();
        return;
    }
    if (isSpotlightMode() && event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_I:
            setSpotlightInverted(!m_spotlightInverted);
            return;
        case Qt::Key_N:
            addSpotlight();
            return;
        case Qt::Key_Delete:
            deleteSpotlight();
            return;
        case Qt::Key_T:
            if (m_tracker) {
                stopTracking();
            } else {
                startTracking();
            }
            return;
        case Qt::Key_A:
            m_autoTrack = !m_autoTrack;
            if (!m_autoTrack) {
                cancelAutoTrack();
            } else if (m_followingMouse) {
                armAutoTrack();
            }
            saveSettings();
            return;
        case Qt::Key_W:
            setAutoSize(!m_autoSize);
            return;
        case Qt::Key_C:
            setApertureShape(ApertureShape::Circle);
            return;
        case Qt::Key_R:
            setApertureShape(ApertureShape::Rectangle);
            return;
        case Qt::Key_F:
            startDrawingOutline();
            return;
        case Qt::Key_Z:
            cycleZoom();
            return;
        default:
            break;
        }
    }

    if (isSpotlightMode() && event->modifiers().testFlag(Qt::AltModifier)) {
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal
            || event->key() == Qt::Key_BracketRight) {
            if (m_apertureShape == ApertureShape::Circle) {
                setSpotlightRadius(m_spotlightRadius + 16);
            } else {
                setSpotlightRectSize(m_spotlightRectSize + QSize(32, 20));
            }
            return;
        }
        if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Underscore
            || event->key() == Qt::Key_BracketLeft) {
            if (m_apertureShape == ApertureShape::Circle) {
                setSpotlightRadius(m_spotlightRadius - 16);
            } else {
                setSpotlightRectSize(m_spotlightRectSize - QSize(32, 20));
            }
            return;
        }
    }

    if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
        setBlurLevel(event->key() - Qt::Key_0);
        return;
    }
    if (event->key() == Qt::Key_0) {
        setBlurLevel(10);
        return;
    }
    if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal
        || event->key() == Qt::Key_BracketRight) {
        setBlurLevel(m_blurLevel + 1);
        return;
    }
    if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Underscore
        || event->key() == Qt::Key_BracketLeft) {
        setBlurLevel(m_blurLevel - 1);
        return;
    }
    QWidget::keyPressEvent(event);
}

void BlurWindow::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (!m_interacting) {
        m_hovered = false;
        m_minimizeHovered = false;
        m_minimizePressed = false;
        m_modeButtonHovered = false;
        m_modeButtonPressed = false;
        m_closeHovered = false;
        m_closePressed = false;
        unsetCursor();
        updateInputShape();
        update();
    }
}

void BlurWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (isSpotlightMode()) {
        if (m_followingMouse && event->button() == Qt::LeftButton) {
            m_followingMouse = false;
            cancelAutoTrack();
            updateInputShape();
            saveSettings();
            update();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && isInsideAperture(event->position())) {
            stopTracking();
            m_followingMouse = true;
            m_spotlightDragOffset = event->position().toPoint() - m_spotlightCenter;
            if (m_autoTrack) {
                armAutoTrack();
            }
            updateInputShape();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    const QPoint position = event->position().toPoint();
    if (event->button() == Qt::LeftButton && handleRect().contains(position)
        && !minimizeButtonRect().contains(position)
        && !modeButtonRect().contains(position)
        && !closeButtonRect().contains(position)) {
        setMode(Mode::Spotlight);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void BlurWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (isSpotlightMode()) {
        if (m_drawingOutline) {
            if (!m_stroke.isEmpty() && QLineF(m_stroke.last(), event->position()).length() >= 3) {
                m_stroke << event->position();
                update();
            }
            event->accept();
            return;
        }
        if (m_followingMouse) {
            if (m_autoTrack) {
                armAutoTrack();
            }
            m_followMoving = true;
            m_followMoveTimer->start();
            m_spotlightCenter = event->position().toPoint() - m_spotlightDragOffset;
            clampSpotlightCenter();
            applyBlur();
            updateInputShape();
            update();
            event->accept();
            return;
        }
        if (m_resizingSpotlight) {
            if (m_apertureShape == ApertureShape::Circle) {
                setSpotlightRadius(qRound(QLineF(QPointF(m_spotlightCenter), event->position()).length()));
            } else {
                const QPoint delta = event->position().toPoint() - m_spotlightResizeStartPosition;
                QRect rect(QPoint(m_spotlightResizeStartCenter.x() - m_spotlightResizeStartSize.width() / 2,
                                  m_spotlightResizeStartCenter.y() - m_spotlightResizeStartSize.height() / 2),
                           m_spotlightResizeStartSize);
                if (m_spotlightResizeEdges.testFlag(LeftEdge)) {
                    rect.setLeft(rect.left() + delta.x());
                }
                if (m_spotlightResizeEdges.testFlag(RightEdge)) {
                    rect.setRight(rect.right() + delta.x());
                }
                if (m_spotlightResizeEdges.testFlag(TopEdge)) {
                    rect.setTop(rect.top() + delta.y());
                }
                if (m_spotlightResizeEdges.testFlag(BottomEdge)) {
                    rect.setBottom(rect.bottom() + delta.y());
                }
                if (rect.width() < kMinimumSpotlightRectWidth) {
                    if (m_spotlightResizeEdges.testFlag(LeftEdge)) {
                        rect.setLeft(rect.right() - kMinimumSpotlightRectWidth);
                    } else {
                        rect.setRight(rect.left() + kMinimumSpotlightRectWidth);
                    }
                }
                if (rect.height() < kMinimumSpotlightRectHeight) {
                    if (m_spotlightResizeEdges.testFlag(TopEdge)) {
                        rect.setTop(rect.bottom() - kMinimumSpotlightRectHeight);
                    } else {
                        rect.setBottom(rect.top() + kMinimumSpotlightRectHeight);
                    }
                }
                m_spotlightCenter = rect.center();
                setSpotlightRectSize(rect.size());
            }
            event->accept();
            return;
        }
        if (m_draggingSpotlight) {
            if (m_autoTrack) {
                m_dragAutoTrack = true;
                armAutoTrack();
            }
            m_spotlightCenter = event->position().toPoint() - m_spotlightDragOffset;
            clampSpotlightCenter();
            applyBlur();
            updateInputShape();
            update();
            event->accept();
            return;
        }
        if (m_dragAutoTrack) {
            cancelAutoTrack();
        }
        selectSpotlight(event->position().toPoint());
        updateCursor(event->position().toPoint());
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (m_clickThrough) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (event->buttons().testFlag(Qt::LeftButton) && m_interacting) {
        return;
    }

    updateCursor(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void BlurWindow::mousePressEvent(QMouseEvent *event)
{
    if (isSpotlightMode()) {
        if (m_drawingOutline) {
            if (event->button() == Qt::LeftButton) {
                m_stroke.clear();
                m_stroke << event->position();
            }
            event->accept();
            return;
        }
        if (m_followingMouse) {
            if (event->button() == Qt::RightButton) {
                showContextMenu(event->globalPosition().toPoint());
            }
            event->accept();
            return;
        }
        if (m_dragAutoTrack) {
            cancelAutoTrack();
        }
        if (event->button() == Qt::RightButton) {
            selectSpotlight(event->position().toPoint());
            showContextMenu(event->globalPosition().toPoint());
            return;
        }
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        selectSpotlight(event->position().toPoint());
        if (isOnSpotlightBorder(event->position())) {
            stopTracking();
            m_resizingSpotlight = true;
            m_spotlightResizeEdges = spotlightRectEdgesAt(event->position().toPoint());
            m_spotlightResizeStartPosition = event->position().toPoint();
            m_spotlightResizeStartCenter = m_spotlightCenter;
            m_spotlightResizeStartSize = m_spotlightRectSize;
            setInteractiveState(true);
            if (m_apertureShape == ApertureShape::Circle) {
                setSpotlightRadius(qRound(QLineF(QPointF(m_spotlightCenter), event->position()).length()));
            }
            event->accept();
            return;
        }
        if (isInsideAperture(event->position())) {
            stopTracking();
            m_draggingSpotlight = true;
            m_spotlightDragOffset = event->position().toPoint() - m_spotlightCenter;
            setInteractiveState(true);
            event->accept();
            return;
        }
        event->accept();
        return;
    }

    if (m_clickThrough) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::RightButton) {
        showContextMenu(event->globalPosition().toPoint());
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (minimizeButtonRect().contains(event->position().toPoint())) {
        m_minimizePressed = true;
        update(minimizeButtonRect());
        event->accept();
        return;
    }

    if (modeButtonRect().contains(event->position().toPoint())) {
        m_modeButtonPressed = true;
        update(modeButtonRect());
        event->accept();
        return;
    }

    if (closeButtonRect().contains(event->position().toPoint())) {
        m_closePressed = true;
        update(closeButtonRect());
        event->accept();
        return;
    }

    setInteractiveState(true);
    m_pressLocalPosition = event->position().toPoint();
    m_pressSize = size();
    m_pressedEdges = edgesAt(m_pressLocalPosition);

    const QPointer<BlurWindow> guard(this);
    if (m_pressedEdges != NoEdge && windowHandle()->startSystemResize(toQtEdges(m_pressedEdges))) {
        if (guard) {
            event->accept();
        }
        return;
    }

    if (handleRect().contains(m_pressLocalPosition) && windowHandle()->startSystemMove()) {
        if (guard) {
            event->accept();
        }
        return;
    }

    event->accept();
}

void BlurWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (isSpotlightMode()) {
        if (m_drawingOutline) {
            if (event->button() == Qt::LeftButton && !m_stroke.isEmpty()) {
                finishOutline();
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            m_resizingSpotlight = false;
            m_spotlightResizeEdges = NoEdge;
            m_draggingSpotlight = false;
            setInteractiveState(false);
            saveSettings();
            update();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const bool minimizeWasPressed = m_minimizePressed;
        m_minimizePressed = false;
        if (minimizeWasPressed) {
            update(minimizeButtonRect());
            if (minimizeButtonRect().contains(event->position().toPoint())) {
                showMinimized();
                event->accept();
                return;
            }
            event->accept();
            return;
        }

        const bool modeButtonWasPressed = m_modeButtonPressed;
        m_modeButtonPressed = false;
        if (modeButtonWasPressed) {
            update(modeButtonRect());
            if (modeButtonRect().contains(event->position().toPoint())) {
                setMode(Mode::Spotlight);
                event->accept();
                return;
            }
            event->accept();
            return;
        }

        const bool closeWasPressed = m_closePressed;
        m_closePressed = false;
        if (closeWasPressed) {
            update(closeButtonRect());
            if (closeButtonRect().contains(event->position().toPoint())) {
                close();
                return;
            }
            event->accept();
            return;
        }
        m_pressedEdges = NoEdge;
        setInteractiveState(false);
        saveSettings();
    }
    QWidget::mouseReleaseEvent(event);
}

void BlurWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (isSpotlightMode()) {
        QPainterPath overlay;
        overlay.addRect(rect());
        QPainterPath aperture = aperturePath(spotlightBounds({m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize}));
        aperture.setFillRule(Qt::WindingFill);
        for (const Spotlight &spotlight : m_otherSpotlights) {
            aperture.addPath(aperturePath(spotlightBounds(spotlight)));
        }
        if (m_spotlightInverted) {
            painter.fillPath(aperture, QColor(255, 255, 255, 1));
        } else {
            painter.fillPath(overlay.subtracted(aperture), QColor(255, 255, 255, 1));
        }
        if ((m_followingMouse || m_dragAutoTrack) && m_autoTrack && m_idleTimer->isActive()
            && kAutoTrackIdleMs - m_idleTimer->remainingTime() >= 800) {
            // Autotrack countdown: one more dot for each second the mouse stands still.
            const int elapsed = kAutoTrackIdleMs - m_idleTimer->remainingTime();
            const int dots = qMin((elapsed + 200) / 1000, 3); // the object is memorized well before the first dot
            QFont font = painter.font();
            font.setPixelSize(40);
            font.setBold(true);
            QPainterPath text;
            const QRectF area = spotlightBounds({m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize});
            text.addText(0, 0, font, QString(dots, QLatin1Char('.')));
            text.translate(area.center() - text.boundingRect().center());
            painter.setPen(QPen(QColor(0, 0, 0, 200), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(text);
            painter.fillPath(text, QColor(255, 255, 255));
        }
        if (m_drawingOutline && m_stroke.size() > 1) {
            // Dotted line: white dots on a dark trail, plus a fainter dotted line back to the start.
            QPen halo(QColor(0, 0, 0, 170), 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            QPen dots(QColor(255, 255, 255), 3, Qt::CustomDashLine, Qt::RoundCap, Qt::RoundJoin);
            dots.setDashPattern({0.1, 2.2});
            painter.setPen(halo);
            painter.drawPolyline(m_stroke);
            painter.setPen(dots);
            painter.drawPolyline(m_stroke);
            dots.setColor(QColor(255, 255, 255, 120));
            dots.setWidth(2);
            painter.setPen(dots);
            painter.drawLine(m_stroke.last(), m_stroke.first());
        }
        return;
    }

    const QRectF bounds = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(bounds, kVisualRadius, kVisualRadius);

    QColor fill = m_tintColor;
    fill.setAlpha(m_tintAlpha);
    painter.fillPath(path, fill);

    if (m_hovered || m_interacting || !m_blurAvailable) {
        QColor border = m_blurAvailable ? QColor(61, 174, 233, m_borderAlpha) : QColor(220, 65, 55, 230);
        painter.setPen(QPen(border, 1.5));
        painter.drawPath(path);

        const QRect grip = handleRect().adjusted(0, 0, -1, -1);
        painter.fillRect(grip, QColor(61, 174, 233, 65));
        painter.setPen(QPen(QColor(61, 174, 233, 180), 1));
        painter.drawRect(grip);
    }

    drawTitleButtons(&painter);
}

void BlurWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (isSpotlightMode()) {
        clampSpotlightCenter();
    }
    applyBlur();
    updateInputShape();
    saveSettings();
}

void BlurWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    raise();
    if (!isSpotlightMode()) {
        activateWindow();
    }
    applyBlur();
    updateInputShape();
    QTimer::singleShot(150, this, &BlurWindow::applyBlur);
    QTimer::singleShot(800, this, &BlurWindow::applyBlur);
}

void BlurWindow::applyBlur()
{
    publishZoomRegions();
    QWindow *window = windowHandle();
    if (!window) {
        return;
    }

    if (isSpotlightMode()) {
        const QRegion apertures = allAperturesRegion();
        const QRegion blurRegion = m_spotlightInverted ? apertures : QRegion(rect()) - apertures;
        KWindowEffects::enableBlurBehind(window, true, blurRegion);
        m_blurAvailable = KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
        update();
        return;
    }

    const QRegion blurRegion(rect());
    KWindowEffects::enableBlurBehind(window, true, blurRegion);
    m_blurAvailable = KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    update();
}

void BlurWindow::loadSettings()
{
    const QSize defaultSize(360, 130);
    resize(m_settings.value(QStringLiteral("size"), defaultSize).toSize().expandedTo(minimumSize()));
    m_tintAlpha = qBound(0, m_settings.value(QStringLiteral("tintAlpha"), 1).toInt(), 90);
    m_borderAlpha = qBound(40, m_settings.value(QStringLiteral("borderAlpha"), 210).toInt(), 255);
    m_blurLevel = qBound(1, m_settings.value(QStringLiteral("blurLevel"), 4).toInt(), 10);
    m_clickThrough = m_settings.value(QStringLiteral("clickThrough"), false).toBool();
    m_autoTrack = m_settings.value(QStringLiteral("autoTrack"), false).toBool();
    m_autoSize = m_settings.value(QStringLiteral("autoSize"), false).toBool();
    m_spotlightRadius = qBound(kMinimumSpotlightRadius,
                               m_settings.value(QStringLiteral("spotlightRadius"), 140).toInt(),
                               kMaximumSpotlightRadius);
    m_spotlightCenter = m_settings.value(QStringLiteral("spotlightCenter"), QPoint(420, 280)).toPoint();
    m_spotlightRectSize = m_settings.value(QStringLiteral("spotlightRectSize"), QSize(320, 190)).toSize()
                              .expandedTo(QSize(kMinimumSpotlightRectWidth, kMinimumSpotlightRectHeight));
    const QString shape = m_settings.value(QStringLiteral("apertureShape"), QStringLiteral("circle")).toString();
    m_apertureShape = shape == QStringLiteral("rectangle") ? ApertureShape::Rectangle : ApertureShape::Circle;
    const QVariantList outline = m_settings.value(QStringLiteral("freeformOutline")).toList();
    for (qsizetype i = 0; i + 1 < outline.size(); i += 2) {
        m_outline << QPointF(outline.at(i).toDouble(), outline.at(i + 1).toDouble());
    }
    if (shape == QStringLiteral("freeform") && m_outline.size() >= 3) {
        m_apertureShape = ApertureShape::Freeform;
    }
    m_spotlightInverted = m_settings.value(QStringLiteral("spotlightInverted"), false).toBool();
}

void BlurWindow::saveSettings()
{
    if (!isSpotlightMode() && !isFullScreen()) {
        m_settings.setValue(QStringLiteral("size"), size());
    }
    m_settings.setValue(QStringLiteral("tintAlpha"), m_tintAlpha);
    m_settings.setValue(QStringLiteral("borderAlpha"), m_borderAlpha);
    m_settings.setValue(QStringLiteral("blurLevel"), m_blurLevel);
    m_settings.setValue(QStringLiteral("clickThrough"), m_clickThrough);
    m_settings.setValue(QStringLiteral("autoTrack"), m_autoTrack);
    m_settings.setValue(QStringLiteral("autoSize"), m_autoSize);
    m_settings.setValue(QStringLiteral("spotlightRadius"), m_spotlightRadius);
    m_settings.setValue(QStringLiteral("spotlightRectSize"), m_spotlightRectSize);
    m_settings.setValue(QStringLiteral("spotlightCenter"), m_spotlightCenter);
    m_settings.setValue(QStringLiteral("apertureShape"),
                        m_apertureShape == ApertureShape::Rectangle ? QStringLiteral("rectangle")
                        : m_apertureShape == ApertureShape::Freeform ? QStringLiteral("freeform") : QStringLiteral("circle"));
    QVariantList outline;
    for (const QPointF &point : m_outline) {
        outline << point.x() << point.y();
    }
    m_settings.setValue(QStringLiteral("freeformOutline"), outline);
    m_settings.setValue(QStringLiteral("spotlightInverted"), m_spotlightInverted);
}

void BlurWindow::setBlurLevel(int level)
{
    const int boundedLevel = qBound(1, level, 10);
    if (boundedLevel == m_blurLevel && level != m_blurLevel) {
        QToolTip::showText(mapToGlobal(rect().center()),
                           QStringLiteral("Blur %1/10").arg(m_blurLevel),
                           this, rect(), 900);
        return;
    }

    m_blurLevel = boundedLevel;
    m_settings.setValue(QStringLiteral("blurLevel"), m_blurLevel);
    m_settings.sync();

    QDBusInterface effectsInterface(QStringLiteral("org.kde.KWin"),
                                    QStringLiteral("/Effects"),
                                    QStringLiteral("org.kde.kwin.Effects"),
                                    QDBusConnection::sessionBus());
    effectsInterface.call(QDBus::NoBlock,
                          QStringLiteral("reconfigureEffect"),
                          QStringLiteral("stableblur"));

    QToolTip::showText(mapToGlobal(rect().center()),
                       QStringLiteral("Blur %1/10").arg(m_blurLevel),
                       this, rect(), 900);
}

void BlurWindow::setClickThrough(bool enabled)
{
    m_clickThrough = enabled;
    saveSettings();
    updateInputShape();
    if (m_trayMenu) {
        const QList<QAction *> actions = m_trayMenu->actions();
        for (QAction *action : actions) {
            if (action->text() == QStringLiteral("Click-through")) {
                action->setChecked(enabled);
                break;
            }
        }
    }
}

void BlurWindow::setMode(Mode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_followingMouse = false;
    cancelAutoTrack();
    stopAllTracking();

    if (mode == Mode::Spotlight) {
        m_normalGeometry = geometry();
        m_clickThrough = false;
        m_apertureShape = ApertureShape::Circle;
        m_spotlightInverted = true;
        m_mode = mode;
        setFocusPolicy(Qt::NoFocus);
        setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
        setMinimumSize(1, 1);
        showFullScreen();
        raise();
        clampSpotlightCenter();
    } else {
        m_mode = mode;
        setWindowFlag(Qt::WindowDoesNotAcceptFocus, false);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(kMinimumWidth, kMinimumHeight);
        showNormal();
        if (m_normalGeometry.isValid()) {
            setGeometry(m_normalGeometry);
        }
        raise();
        activateWindow();
    }

    applyBlur();
    updateInputShape();
    saveSettings();
    update();
}

void BlurWindow::setSpotlightRadius(int radius)
{
    m_spotlightRadius = qBound(kMinimumSpotlightRadius, radius, kMaximumSpotlightRadius);
    clampSpotlightCenter();
    applyBlur();
    updateInputShape();
    saveSettings();
    update();
}

void BlurWindow::setApertureShape(ApertureShape shape)
{
    if (m_apertureShape == shape) {
        return;
    }
    m_apertureShape = shape;
    clampSpotlightCenter();
    applyBlur();
    updateInputShape();
    saveSettings();
    update();
}

void BlurWindow::setSpotlightInverted(bool inverted)
{
    if (m_spotlightInverted == inverted) {
        return;
    }
    m_spotlightInverted = inverted;
    applyBlur();
    updateInputShape();
    saveSettings();
    update();
}

void BlurWindow::setSpotlightRectSize(const QSize &size)
{
    m_spotlightRectSize = size.expandedTo(QSize(kMinimumSpotlightRectWidth, kMinimumSpotlightRectHeight));
    m_spotlightRectSize.setWidth(qMin(m_spotlightRectSize.width(), qMax(kMinimumSpotlightRectWidth, width())));
    m_spotlightRectSize.setHeight(qMin(m_spotlightRectSize.height(), qMax(kMinimumSpotlightRectHeight, height())));
    clampSpotlightCenter();
    applyBlur();
    updateInputShape();
    saveSettings();
    update();
}

void BlurWindow::setInteractiveState(bool active)
{
    m_interacting = active;
    m_hovered = active || underMouse();
    updateInputShape();
    update();
}

void BlurWindow::updateCursor(const QPoint &position)
{
    const bool minimizeHovered = !isSpotlightMode() && minimizeButtonRect().contains(position);
    if (m_minimizeHovered != minimizeHovered) {
        m_minimizeHovered = minimizeHovered;
        update(minimizeButtonRect().adjusted(-2, -2, 2, 2));
    }
    const bool modeButtonHovered = !isSpotlightMode() && modeButtonRect().contains(position);
    if (m_modeButtonHovered != modeButtonHovered) {
        m_modeButtonHovered = modeButtonHovered;
        update(modeButtonRect().adjusted(-2, -2, 2, 2));
    }
    const bool closeHovered = !isSpotlightMode() && closeButtonRect().contains(position);
    if (m_closeHovered != closeHovered) {
        m_closeHovered = closeHovered;
        update(closeButtonRect().adjusted(-2, -2, 2, 2));
    }
    if (minimizeHovered || modeButtonHovered || closeHovered) {
        setCursor(Qt::PointingHandCursor);
        return;
    }

    if (isSpotlightMode()) {
        if (m_resizingSpotlight || isOnSpotlightBorder(position)) {
            const Edges edges = m_resizingSpotlight ? m_spotlightResizeEdges : spotlightRectEdgesAt(position);
            if ((edges.testFlag(LeftEdge) && edges.testFlag(TopEdge)) || (edges.testFlag(RightEdge) && edges.testFlag(BottomEdge))) {
                setCursor(Qt::SizeFDiagCursor);
            } else if ((edges.testFlag(RightEdge) && edges.testFlag(TopEdge)) || (edges.testFlag(LeftEdge) && edges.testFlag(BottomEdge))) {
                setCursor(Qt::SizeBDiagCursor);
            } else if (edges.testFlag(LeftEdge) || edges.testFlag(RightEdge)) {
                setCursor(Qt::SizeHorCursor);
            } else if (edges.testFlag(TopEdge) || edges.testFlag(BottomEdge)) {
                setCursor(Qt::SizeVerCursor);
            } else {
                setCursor(Qt::SizeFDiagCursor);
            }
        } else if (isInsideAperture(position)) {
            setCursor(Qt::SizeAllCursor);
        } else {
            unsetCursor();
        }
        return;
    }

    const Edges edges = edgesAt(position);
    if ((edges.testFlag(LeftEdge) && edges.testFlag(TopEdge)) || (edges.testFlag(RightEdge) && edges.testFlag(BottomEdge))) {
        setCursor(Qt::SizeFDiagCursor);
    } else if ((edges.testFlag(RightEdge) && edges.testFlag(TopEdge)) || (edges.testFlag(LeftEdge) && edges.testFlag(BottomEdge))) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges.testFlag(LeftEdge) || edges.testFlag(RightEdge)) {
        setCursor(Qt::SizeHorCursor);
    } else if (edges.testFlag(TopEdge) || edges.testFlag(BottomEdge)) {
        setCursor(Qt::SizeVerCursor);
    } else if (handleRect().contains(position)) {
        setCursor(Qt::SizeAllCursor);
    } else {
        unsetCursor();
    }
}

void BlurWindow::updateInputShape()
{
    if (isSpotlightMode()) {
        // QWindow's Wayland mask sets the input region without clipping painting.
        // Only the apertures take input, so other windows stay usable, also while following the mouse.
        if (windowHandle()) {
            windowHandle()->setMask(m_drawingOutline ? QRegion(rect()) : spotlightInputRegion());
        }
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        return;
    }

    if (windowHandle()) {
        windowHandle()->setMask(QRegion());
    }
    if (m_clickThrough && !m_interacting) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        unsetCursor();
        return;
    }

    setAttribute(Qt::WA_TransparentForMouseEvents, false);
}

BlurWindow::Edges BlurWindow::edgesAt(const QPoint &position) const
{
    if (isSpotlightMode()) {
        return NoEdge;
    }
    if (minimizeButtonRect().contains(position)
        || modeButtonRect().contains(position)
        || closeButtonRect().contains(position)) {
        return NoEdge;
    }

    Edges edges = NoEdge;
    if (position.x() <= kResizeMargin) {
        edges |= LeftEdge;
    }
    if (position.y() <= kResizeMargin) {
        edges |= TopEdge;
    }
    if (position.x() >= width() - kResizeMargin) {
        edges |= RightEdge;
    }
    if (position.y() >= height() - kResizeMargin) {
        edges |= BottomEdge;
    }
    return edges;
}

BlurWindow::Edges BlurWindow::spotlightRectEdgesAt(const QPoint &position) const
{
    if (m_apertureShape == ApertureShape::Circle) {
        return NoEdge;
    }

    const QRect rect = spotlightRectangleRect().toAlignedRect();
    if (!rect.adjusted(-kSpotlightResizeMargin, -kSpotlightResizeMargin, kSpotlightResizeMargin, kSpotlightResizeMargin).contains(position)) {
        return NoEdge;
    }

    Edges edges = NoEdge;
    if (qAbs(position.x() - rect.left()) <= kSpotlightResizeMargin) {
        edges |= LeftEdge;
    }
    if (qAbs(position.x() - rect.right()) <= kSpotlightResizeMargin) {
        edges |= RightEdge;
    }
    if (qAbs(position.y() - rect.top()) <= kSpotlightResizeMargin) {
        edges |= TopEdge;
    }
    if (qAbs(position.y() - rect.bottom()) <= kSpotlightResizeMargin) {
        edges |= BottomEdge;
    }
    return edges;
}

QRegion BlurWindow::apertureRegion() const
{
    return apertureRegionIn(spotlightBounds({m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize}));
}

QPainterPath BlurWindow::aperturePath(const QRectF &bounds) const
{
    QPainterPath path;
    if (m_apertureShape == ApertureShape::Circle) {
        path.addEllipse(bounds);
    } else if (m_apertureShape == ApertureShape::Freeform && m_outline.size() >= 3) {
        QPolygonF polygon;
        for (const QPointF &point : m_outline) {
            polygon << QPointF(bounds.x() + point.x() * bounds.width(), bounds.y() + point.y() * bounds.height());
        }
        path.addPolygon(polygon);
        path.closeSubpath();
    } else {
        path.addRect(bounds);
    }
    return path;
}

QRegion BlurWindow::apertureRegionIn(const QRectF &bounds) const
{
    if (m_apertureShape == ApertureShape::Circle) {
        return QRegion(bounds.toAlignedRect(), QRegion::Ellipse);
    }
    if (m_apertureShape == ApertureShape::Freeform && m_outline.size() >= 3) {
        return QRegion(aperturePath(bounds).toFillPolygon().toPolygon(), Qt::WindingFill);
    }
    return QRegion(bounds.toAlignedRect());
}

void BlurWindow::startDrawingOutline()
{
    if (!isSpotlightMode()) {
        return;
    }
    m_followingMouse = false;
    cancelAutoTrack();
    m_drawingOutline = true;
    m_stroke.clear();
    setCursor(Qt::CrossCursor);
    updateInputShape();
    update();
}

void BlurWindow::cancelOutline()
{
    m_drawingOutline = false;
    m_stroke.clear();
    unsetCursor();
    updateInputShape();
    update();
}

// The drawn stroke becomes the aperture of the current spotlight, placed where it was drawn.
void BlurWindow::finishOutline()
{
    const QRectF box = m_stroke.boundingRect();
    if (m_stroke.size() < 8 || box.width() < kMinimumSpotlightRectWidth / 2 || box.height() < kMinimumSpotlightRectHeight / 2) {
        cancelOutline();
        return;
    }
    // Keep at most about 200 points.
    const int step = qMax<qsizetype>(1, m_stroke.size() / 200);
    QPolygonF outline;
    for (qsizetype i = 0; i < m_stroke.size(); i += step) {
        outline << QPointF((m_stroke.at(i).x() - box.x()) / box.width(), (m_stroke.at(i).y() - box.y()) / box.height());
    }
    m_outline = outline;
    m_apertureShape = ApertureShape::Freeform;
    m_spotlightRectSize = box.size().toSize().expandedTo(QSize(kMinimumSpotlightRectWidth, kMinimumSpotlightRectHeight));
    m_spotlightRectSize.setWidth(qMin(m_spotlightRectSize.width(), width()));
    m_spotlightRectSize.setHeight(qMin(m_spotlightRectSize.height(), height()));
    m_spotlightCenter = box.center().toPoint();
    m_spotlightZoom = 1.0;
    m_drawingOutline = false;
    m_stroke.clear();
    unsetCursor();
    clampSpotlightCenter();
    applyBlur();
    updateInputShape();
    saveSettings();
    update();
}

QRegion BlurWindow::spotlightInputRegion() const
{
    QRegion region;
    for (const Spotlight &spotlight : m_otherSpotlights) {
        const QRect bounds = spotlightBounds(spotlight)
            .adjusted(-kSpotlightResizeMargin, -kSpotlightResizeMargin,
                      kSpotlightResizeMargin, kSpotlightResizeMargin).toAlignedRect();
        region += QRegion(bounds, m_apertureShape == ApertureShape::Circle ? QRegion::Ellipse : QRegion::Rectangle);
    }
    if (m_followingMouse && m_followMoving) {
        const QRect wide = (m_apertureShape == ApertureShape::Circle ? spotlightRect() : spotlightRectangleRect())
                               .adjusted(-kFollowMargin, -kFollowMargin, kFollowMargin, kFollowMargin).toAlignedRect();
        region += QRegion(wide);
    }
    {
        if (m_apertureShape == ApertureShape::Circle) {
            region += QRegion(spotlightRect()
                                  .adjusted(-kSpotlightResizeMargin, -kSpotlightResizeMargin, kSpotlightResizeMargin, kSpotlightResizeMargin)
                                  .toAlignedRect(),
                              QRegion::Ellipse);
        } else {
            region += QRegion(spotlightRectangleRect()
                                  .adjusted(-kSpotlightResizeMargin, -kSpotlightResizeMargin, kSpotlightResizeMargin, kSpotlightResizeMargin)
                                  .toAlignedRect());
        }
    }
    return region;
}

QRect BlurWindow::minimizeButtonRect() const
{
    return QRect(width() - (kTitleButtonWidth * 3),
                 0,
                 kTitleButtonWidth,
                 kTitleBarHeight);
}

QRect BlurWindow::modeButtonRect() const
{
    return QRect(width() - (kTitleButtonWidth * 2),
                 0,
                 kTitleButtonWidth,
                 kTitleBarHeight);
}

QRect BlurWindow::closeButtonRect() const
{
    return QRect(width() - kTitleButtonWidth,
                 0,
                 kTitleButtonWidth,
                 kTitleBarHeight);
}

QRect BlurWindow::handleRect() const
{
    return QRect(0, 0, width(), qMin(kTitleBarHeight, height()));
}

QRectF BlurWindow::spotlightRect() const
{
    return QRectF(m_spotlightCenter.x() - m_spotlightRadius,
                  m_spotlightCenter.y() - m_spotlightRadius,
                  m_spotlightRadius * 2,
                  m_spotlightRadius * 2);
}

QRectF BlurWindow::spotlightRectangleRect() const
{
    return QRectF(m_spotlightCenter.x() - m_spotlightRectSize.width() / 2.0,
                  m_spotlightCenter.y() - m_spotlightRectSize.height() / 2.0,
                  m_spotlightRectSize.width(),
                  m_spotlightRectSize.height());
}

bool BlurWindow::isSpotlightMode() const
{
    return m_mode == Mode::Spotlight;
}

bool BlurWindow::isOnSpotlightBorder(const QPointF &position) const
{
    if (!isSpotlightMode()) {
        return false;
    }
    if (m_apertureShape != ApertureShape::Circle) {
        return spotlightRectEdgesAt(position.toPoint()) != NoEdge;
    }
    const qreal distance = QLineF(QPointF(m_spotlightCenter), position).length();
    return qAbs(distance - m_spotlightRadius) <= kSpotlightResizeMargin;
}

bool BlurWindow::isInsideAperture(const QPointF &position) const
{
    if (m_apertureShape == ApertureShape::Circle) {
        return QLineF(QPointF(m_spotlightCenter), position).length() <= m_spotlightRadius;
    }
    if (m_apertureShape == ApertureShape::Freeform) {
        return aperturePath(spotlightRectangleRect()).contains(position);
    }
    return spotlightRectangleRect().contains(position);
}

QRectF BlurWindow::spotlightBounds(const Spotlight &spotlight) const
{
    const QSizeF size = m_apertureShape == ApertureShape::Circle
        ? QSizeF(spotlight.radius * 2, spotlight.radius * 2) : QSizeF(spotlight.size);
    return QRectF(QPointF(spotlight.center) - QPointF(size.width() / 2, size.height() / 2), size);
}

QRegion BlurWindow::allAperturesRegion() const
{
    QRegion region = apertureRegion();
    for (const Spotlight &spotlight : m_otherSpotlights) {
        region += apertureRegionIn(spotlightBounds(spotlight));
    }
    return region;
}

void BlurWindow::addSpotlight()
{
    if (m_otherSpotlights.size() >= kMaxSpotlights - 1) {
        return;
    }
    cancelAutoTrack();
    m_otherSpotlights.append({m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize, m_spotlightZoom, m_tracker});
    m_tracker = nullptr;
    m_spotlightZoom = 1.0;
    m_spotlightCenter += QPoint(m_spotlightRadius + 60, 60);
    m_spotlightRadius = qMax(kMinimumSpotlightRadius, qRound(m_spotlightRadius * 0.7));
    m_spotlightRectSize = (m_spotlightRectSize * 0.7)
        .expandedTo(QSize(kMinimumSpotlightRectWidth, kMinimumSpotlightRectHeight));
    clampSpotlightCenter();
    applyBlur();
    updateInputShape();
    update();
}

void BlurWindow::selectSpotlight(const QPoint &position)
{
    if (isInsideAperture(position) || isOnSpotlightBorder(position)) {
        return;
    }
    for (qsizetype i = m_otherSpotlights.size(); i-- > 0;) {
        const Spotlight candidate = m_otherSpotlights.at(i);
        const QRect bounds = spotlightBounds(candidate)
            .adjusted(-kSpotlightResizeMargin, -kSpotlightResizeMargin,
                      kSpotlightResizeMargin, kSpotlightResizeMargin).toAlignedRect();
        if (QRegion(bounds, m_apertureShape == ApertureShape::Circle
                        ? QRegion::Ellipse : QRegion::Rectangle).contains(position)) {
            m_otherSpotlights[i] = {m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize, m_spotlightZoom, m_tracker};
            m_spotlightCenter = candidate.center;
            m_spotlightRadius = candidate.radius;
            m_spotlightRectSize = candidate.size;
            m_spotlightZoom = candidate.zoom;
            m_tracker = candidate.tracker;
            return;
        }
    }
}

void BlurWindow::deleteSpotlight()
{
    cancelAutoTrack();
    stopTracking();
    if (m_otherSpotlights.isEmpty()) {
        close();
        return;
    }
    const Spotlight next = m_otherSpotlights.takeLast();
    m_spotlightCenter = next.center;
    m_spotlightRadius = next.radius;
    m_spotlightRectSize = next.size;
    m_spotlightZoom = next.zoom;
    m_tracker = next.tracker;
    applyBlur();
    updateInputShape();
    update();
}

void BlurWindow::publishZoomRegions()
{
    QJsonArray apertures;
    auto append = [this, &apertures](const Spotlight &spotlight) {
        const QRectF bounds = spotlightBounds(spotlight);
        const double zoom = m_apertureShape == ApertureShape::Freeform ? 1.0 : spotlight.zoom; // no zoom for hand-drawn shapes
        apertures.append(QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height(), zoom});
    };
    if (isSpotlightMode() && isVisible()) {
        for (const Spotlight &spotlight : m_otherSpotlights) {
            append(spotlight);
        }
        append({m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize, m_spotlightZoom});
    }
    QJsonArray outline; // hand-drawn shape: at most 64 points, the effect cuts the apertures along them
    if (m_apertureShape == ApertureShape::Freeform && m_outline.size() >= 3) {
        const int step = (m_outline.size() + 63) / 64;
        for (qsizetype i = 0; i < m_outline.size(); i += step) {
            outline.append(m_outline.at(i).x());
            outline.append(m_outline.at(i).y());
        }
    }
    const QJsonObject payload{{QStringLiteral("apertures"), apertures},
                              {QStringLiteral("circle"), m_apertureShape == ApertureShape::Circle},
                              {QStringLiteral("outline"), outline},
                              {QStringLiteral("inverted"), m_spotlightInverted}};
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
        QStringLiteral("/org/local/StableBlur"), QStringLiteral("org.local.StableBlur"), QStringLiteral("setZoomRegions"));
    message << QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QDBusConnection::sessionBus().asyncCall(message);
}

void BlurWindow::clampSpotlightCenter()
{
    if (width() <= 0 || height() <= 0) {
        return;
    }

    m_spotlightCenter = clampedCenter(m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize);
}

QPoint BlurWindow::clampedCenter(QPoint center, int radius, const QSize &size) const
{
    const int xMargin = m_apertureShape == ApertureShape::Circle ? radius : size.width() / 2;
    const int yMargin = m_apertureShape == ApertureShape::Circle ? radius : size.height() / 2;
    center.setX(qBound(xMargin, center.x(), qMax(xMargin, width() - xMargin)));
    center.setY(qBound(yMargin, center.y(), qMax(yMargin, height() - yMargin)));
    return center;
}

void BlurWindow::drawTitleButtons(QPainter *painter)
{
    drawTitleButton(this,
                    painter,
                    minimizeButtonRect(),
                    m_minimizeHovered,
                    m_minimizePressed,
                    QStyle::SP_TitleBarMinButton);
    drawTitleButton(this,
                    painter,
                    modeButtonRect(),
                    m_modeButtonHovered,
                    m_modeButtonPressed,
                    isSpotlightMode() ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton);
    drawTitleButton(this,
                    painter,
                    closeButtonRect(),
                    m_closeHovered,
                    m_closePressed,
                    QStyle::SP_TitleBarCloseButton);
}
