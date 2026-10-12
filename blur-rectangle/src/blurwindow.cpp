#include "blurwindow.h"

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
#include <QToolButton>
#include <QWindow>

namespace
{
constexpr int kMinimumWidth = 160;
constexpr int kMinimumHeight = 56;
constexpr int kResizeMargin = 10;
constexpr int kVisualRadius = 5;
constexpr int kTitleButtonWidth = 46;
constexpr int kTitleBarHeight = 30;
constexpr int kSpotlightResizeMargin = 10;
constexpr int kShapeButtonSize = 38;
constexpr int kShapeButtonGap = 8;
constexpr int kShapeButtonMargin = 14;
constexpr int kMinimumSpotlightRadius = 48;
constexpr int kMaximumSpotlightRadius = 520;
constexpr int kMinimumSpotlightRectWidth = 96;
constexpr int kMinimumSpotlightRectHeight = 64;

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
{
    setWindowTitle(QStringLiteral("Blur Rectangle"));
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    loadSettings();
    m_addButton = new QToolButton(this);
    m_addButton->setIcon(QIcon::fromTheme(QStringLiteral("list-add"), style()->standardIcon(QStyle::SP_FileDialogNewFolder)));
    m_addButton->setToolTip(QStringLiteral("Add spotlight"));
    m_addButton->setAccessibleName(QStringLiteral("Add spotlight"));
    m_addButton->setFocusPolicy(Qt::NoFocus);
    m_addButton->setFixedSize(kShapeButtonSize, kShapeButtonSize);
    m_addButton->hide();
    connect(m_addButton, &QToolButton::clicked, this, &BlurWindow::addSpotlight);
    createTrayIcon();
}

BlurWindow::~BlurWindow()
{
    saveSettings();
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
    publishZoomRegions();
    saveSettings();
    QWidget::hideEvent(event);
}

void BlurWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
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
    if (isSpotlightMode() && event->key() == Qt::Key_I && event->modifiers() == Qt::NoModifier) {
        setSpotlightInverted(!m_spotlightInverted);
        return;
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
        m_circleShapeHovered = false;
        m_circleShapePressed = false;
        m_rectangleShapeHovered = false;
        m_rectangleShapePressed = false;
        m_invertHovered = false;
        m_invertPressed = false;
        unsetCursor();
        updateInputShape();
        update();
    }
}

void BlurWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (isSpotlightMode()) {
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
            m_spotlightCenter = event->position().toPoint() - m_spotlightDragOffset;
            clampSpotlightCenter();
            applyBlur();
            updateInputShape();
            update();
            event->accept();
            return;
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
        if (event->button() == Qt::RightButton) {
            selectSpotlight(event->position().toPoint());
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
        if (circleShapeButtonRect().contains(event->position().toPoint())) {
            m_circleShapePressed = true;
            update(circleShapeButtonRect());
            event->accept();
            return;
        }
        if (rectangleShapeButtonRect().contains(event->position().toPoint())) {
            m_rectangleShapePressed = true;
            update(rectangleShapeButtonRect());
            event->accept();
            return;
        }
        if (invertButtonRect().contains(event->position().toPoint())) {
            m_invertPressed = true;
            update(invertButtonRect());
            event->accept();
            return;
        }
        selectSpotlight(event->position().toPoint());
        if (isOnSpotlightBorder(event->position())) {
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
        if (event->button() == Qt::LeftButton) {
            const bool minimizeWasPressed = m_minimizePressed;
            m_minimizePressed = false;
            const bool modeButtonWasPressed = m_modeButtonPressed;
            m_modeButtonPressed = false;
            const bool closeWasPressed = m_closePressed;
            m_closePressed = false;
            const bool circleWasPressed = m_circleShapePressed;
            const bool rectangleWasPressed = m_rectangleShapePressed;
            const bool invertWasPressed = m_invertPressed;
            m_circleShapePressed = false;
            m_rectangleShapePressed = false;
            m_invertPressed = false;
            m_resizingSpotlight = false;
            m_spotlightResizeEdges = NoEdge;
            m_draggingSpotlight = false;
            setInteractiveState(false);
            saveSettings();
            if (minimizeWasPressed && minimizeButtonRect().contains(event->position().toPoint())) {
                showMinimized();
                event->accept();
                return;
            }
            if (modeButtonWasPressed && modeButtonRect().contains(event->position().toPoint())) {
                setMode(Mode::BlurRectangle);
                event->accept();
                return;
            }
            if (closeWasPressed && closeButtonRect().contains(event->position().toPoint())) {
                close();
                return;
            }
            if (circleWasPressed && circleShapeButtonRect().contains(event->position().toPoint())) {
                setApertureShape(ApertureShape::Circle);
                event->accept();
                return;
            }
            if (rectangleWasPressed && rectangleShapeButtonRect().contains(event->position().toPoint())) {
                setApertureShape(ApertureShape::Rectangle);
                event->accept();
                return;
            }
            if (invertWasPressed && invertButtonRect().contains(event->position().toPoint())) {
                setSpotlightInverted(!m_spotlightInverted);
                event->accept();
                return;
            }
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
        QPainterPath aperture;
        if (m_apertureShape == ApertureShape::Circle) {
            aperture.addEllipse(spotlightRect());
        } else {
            aperture.addRect(spotlightRectangleRect());
        }
        aperture.setFillRule(Qt::WindingFill);
        for (const Spotlight &spotlight : m_otherSpotlights) {
            if (m_apertureShape == ApertureShape::Circle) {
                aperture.addEllipse(spotlightBounds(spotlight));
            } else {
                aperture.addRect(spotlightBounds(spotlight));
            }
        }
        if (m_spotlightInverted) {
            painter.fillPath(aperture, QColor(255, 255, 255, 1));
        } else {
            painter.fillPath(overlay.subtracted(aperture), QColor(255, 255, 255, 1));
        }

        drawShapeButtons(&painter);
        drawTitleButtons(&painter);
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
    if (m_addButton) {
        m_addButton->move(invertButtonRect().translated(kShapeButtonSize + kShapeButtonGap, 0).topLeft());
    }
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

void BlurWindow::createTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    auto *menu = new QMenu(this);
    m_trayMenu = menu;

    QAction *showHideAction = menu->addAction(QStringLiteral("Show/Hide"));
    connect(showHideAction, &QAction::triggered, this, [this] {
        setVisible(!isVisible());
        if (isVisible()) {
            raise();
            if (!isSpotlightMode()) {
                activateWindow();
            }
        }
    });

    QAction *rectangleModeAction = menu->addAction(QStringLiteral("Blur rectangle mode"));
    connect(rectangleModeAction, &QAction::triggered, this, [this] {
        setMode(Mode::BlurRectangle);
    });

    QAction *spotlightModeAction = menu->addAction(QStringLiteral("Spotlight blocker mode"));
    connect(spotlightModeAction, &QAction::triggered, this, [this] {
        setMode(Mode::Spotlight);
    });
    connect(menu, &QMenu::aboutToShow, this, [this, rectangleModeAction, spotlightModeAction] {
        rectangleModeAction->setVisible(isSpotlightMode());
        spotlightModeAction->setVisible(!isSpotlightMode());
    });

    QAction *clickThroughAction = menu->addAction(QStringLiteral("Click-through"));
    clickThroughAction->setCheckable(true);
    clickThroughAction->setChecked(m_clickThrough);
    connect(clickThroughAction, &QAction::toggled, this, &BlurWindow::setClickThrough);

    menu->addSeparator();
    QAction *quitAction = menu->addAction(QStringLiteral("Quit"));
    connect(quitAction, &QAction::triggered, qApp, &QCoreApplication::quit);

    auto *trayIcon = new QSystemTrayIcon(this);
    m_trayIcon = trayIcon;
    trayIcon->setToolTip(QStringLiteral("Blur Rectangle"));
    trayIcon->setIcon(style()->standardIcon(QStyle::SP_TitleBarShadeButton));
    trayIcon->setContextMenu(menu);
    connect(trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            setVisible(!isVisible());
            if (isVisible()) {
                raise();
                if (!isSpotlightMode()) {
                    activateWindow();
                }
            }
        }
    });
    trayIcon->show();
}

void BlurWindow::loadSettings()
{
    const QSize defaultSize(360, 130);
    resize(m_settings.value(QStringLiteral("size"), defaultSize).toSize().expandedTo(minimumSize()));
    m_tintAlpha = qBound(0, m_settings.value(QStringLiteral("tintAlpha"), 1).toInt(), 90);
    m_borderAlpha = qBound(40, m_settings.value(QStringLiteral("borderAlpha"), 210).toInt(), 255);
    m_blurLevel = qBound(1, m_settings.value(QStringLiteral("blurLevel"), 4).toInt(), 10);
    m_clickThrough = m_settings.value(QStringLiteral("clickThrough"), false).toBool();
    m_spotlightRadius = qBound(kMinimumSpotlightRadius,
                               m_settings.value(QStringLiteral("spotlightRadius"), 140).toInt(),
                               kMaximumSpotlightRadius);
    m_spotlightCenter = m_settings.value(QStringLiteral("spotlightCenter"), QPoint(420, 280)).toPoint();
    m_spotlightRectSize = m_settings.value(QStringLiteral("spotlightRectSize"), QSize(320, 190)).toSize()
                              .expandedTo(QSize(kMinimumSpotlightRectWidth, kMinimumSpotlightRectHeight));
    m_apertureShape = m_settings.value(QStringLiteral("apertureShape"), QStringLiteral("circle")).toString() == QStringLiteral("rectangle")
        ? ApertureShape::Rectangle
        : ApertureShape::Circle;
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
    m_settings.setValue(QStringLiteral("spotlightRadius"), m_spotlightRadius);
    m_settings.setValue(QStringLiteral("spotlightRectSize"), m_spotlightRectSize);
    m_settings.setValue(QStringLiteral("spotlightCenter"), m_spotlightCenter);
    m_settings.setValue(QStringLiteral("apertureShape"),
                        m_apertureShape == ApertureShape::Rectangle ? QStringLiteral("rectangle") : QStringLiteral("circle"));
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
    m_addButton->setVisible(mode == Mode::Spotlight);

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

void BlurWindow::showContextMenu(const QPoint &globalPosition)
{
    QMenu menu(this);
    QAction *deleteAction = nullptr;
    QActionGroup zoomActions(&menu);
    zoomActions.setExclusive(true);
    const QPoint local = mapFromGlobal(globalPosition);
    if (isSpotlightMode() && (isInsideAperture(local) || isOnSpotlightBorder(local))) {
        deleteAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Delete spotlight"));
        QMenu *zoomMenu = menu.addMenu(QIcon::fromTheme(QStringLiteral("zoom-in")), QStringLiteral("Zoom"));
        for (double factor : {1.0, 1.15, 1.25, 1.5, 2.0, 3.0}) {
            QAction *action = zoomMenu->addAction(factor == 1.0 ? QStringLiteral("Off (1x)")
                                                              : QStringLiteral("%1x").arg(factor));
            action->setCheckable(true);
            action->setData(factor);
            action->setChecked(qFuzzyCompare(m_spotlightZoom, factor));
            zoomActions.addAction(action);
        }
        const QDBusInterface zoomEffect(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/local/StableBlur"),
                                        QStringLiteral("org.local.StableBlur"));
        zoomMenu->setEnabled(zoomEffect.isValid() && !m_spotlightInverted);
        if (m_spotlightInverted) {
            zoomMenu->menuAction()->setToolTip(QStringLiteral("Zoom is available when the spotlight is clear"));
            menu.setToolTipsVisible(true);
        }
        if (!zoomEffect.isValid()) {
            zoomMenu->menuAction()->setToolTip(QStringLiteral("Install and reload the updated Stable Blur effect to enable zoom"));
            menu.setToolTipsVisible(true);
        }
        menu.addSeparator();
    }
    QAction *hideAction = menu.addAction(QStringLiteral("Hide"));
    QAction *rectangleModeAction = menu.addAction(QStringLiteral("Blur rectangle mode"));
    QAction *spotlightModeAction = menu.addAction(QStringLiteral("Spotlight blocker mode"));
    rectangleModeAction->setVisible(isSpotlightMode());
    spotlightModeAction->setVisible(!isSpotlightMode());
    QAction *clickThroughAction = menu.addAction(QStringLiteral("Click-through"));
    clickThroughAction->setCheckable(true);
    clickThroughAction->setChecked(m_clickThrough);
    clickThroughAction->setEnabled(!isSpotlightMode());
    menu.addSeparator();
    QAction *quitAction = menu.addAction(QStringLiteral("Quit"));

    QAction *chosen = menu.exec(globalPosition);
    if (!chosen) {
        return;
    }
    if (chosen == deleteAction) {
        deleteSpotlight();
    } else if (zoomActions.actions().contains(chosen)) {
        m_spotlightZoom = chosen->data().toDouble();
        QTimer::singleShot(0, this, &BlurWindow::applyBlur);
    } else if (chosen == hideAction) {
        hide();
    } else if (chosen == rectangleModeAction) {
        setMode(Mode::BlurRectangle);
    } else if (chosen == spotlightModeAction) {
        setMode(Mode::Spotlight);
    } else if (chosen == clickThroughAction) {
        setClickThrough(clickThroughAction->isChecked());
    } else if (chosen == quitAction) {
        qApp->quit();
    }
}

void BlurWindow::updateCursor(const QPoint &position)
{
    const bool minimizeHovered = minimizeButtonRect().contains(position);
    if (m_minimizeHovered != minimizeHovered) {
        m_minimizeHovered = minimizeHovered;
        update(minimizeButtonRect().adjusted(-2, -2, 2, 2));
    }
    const bool modeButtonHovered = modeButtonRect().contains(position);
    if (m_modeButtonHovered != modeButtonHovered) {
        m_modeButtonHovered = modeButtonHovered;
        update(modeButtonRect().adjusted(-2, -2, 2, 2));
    }
    const bool closeHovered = closeButtonRect().contains(position);
    if (m_closeHovered != closeHovered) {
        m_closeHovered = closeHovered;
        update(closeButtonRect().adjusted(-2, -2, 2, 2));
    }
    if (minimizeHovered || modeButtonHovered || closeHovered) {
        setCursor(Qt::PointingHandCursor);
        return;
    }

    if (isSpotlightMode()) {
        const bool circleHovered = circleShapeButtonRect().contains(position);
        const bool rectangleHovered = rectangleShapeButtonRect().contains(position);
        const bool invertHovered = invertButtonRect().contains(position);
        if (m_circleShapeHovered != circleHovered) {
            m_circleShapeHovered = circleHovered;
            update(circleShapeButtonRect().adjusted(-2, -2, 2, 2));
        }
        if (m_rectangleShapeHovered != rectangleHovered) {
            m_rectangleShapeHovered = rectangleHovered;
            update(rectangleShapeButtonRect().adjusted(-2, -2, 2, 2));
        }
        if (m_invertHovered != invertHovered) {
            m_invertHovered = invertHovered;
            update(invertButtonRect().adjusted(-2, -2, 2, 2));
        }
        if (circleHovered || rectangleHovered || invertHovered) {
            setCursor(Qt::PointingHandCursor);
        } else if (m_resizingSpotlight || isOnSpotlightBorder(position)) {
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
        if (windowHandle()) {
            windowHandle()->setMask(spotlightInputRegion());
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
    if (m_apertureShape != ApertureShape::Rectangle) {
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
    if (m_apertureShape == ApertureShape::Circle) {
        return QRegion(spotlightRect().toAlignedRect(), QRegion::Ellipse);
    }
    return QRegion(spotlightRectangleRect().toAlignedRect());
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
    if (m_addButton) {
        region += m_addButton->geometry();
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

    // Keep the interior interactive in either blur mode so it can be dragged.
    region += QRegion(minimizeButtonRect().adjusted(-2, 0, 2, 2));
    region += QRegion(modeButtonRect().adjusted(-2, 0, 2, 2));
    region += QRegion(closeButtonRect().adjusted(-2, 0, 2, 2));
    region += QRegion(circleShapeButtonRect().adjusted(-2, -2, 2, 2));
    region += QRegion(rectangleShapeButtonRect().adjusted(-2, -2, 2, 2));
    region += QRegion(invertButtonRect().adjusted(-2, -2, 2, 2));
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

QRect BlurWindow::circleShapeButtonRect() const
{
    return QRect(kShapeButtonMargin,
                 height() - kShapeButtonMargin - kShapeButtonSize,
                 kShapeButtonSize,
                 kShapeButtonSize);
}

QRect BlurWindow::rectangleShapeButtonRect() const
{
    return circleShapeButtonRect().translated(kShapeButtonSize + kShapeButtonGap, 0);
}

QRect BlurWindow::invertButtonRect() const
{
    return rectangleShapeButtonRect().translated(kShapeButtonSize + kShapeButtonGap, 0);
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
    if (m_apertureShape == ApertureShape::Rectangle) {
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
        region += QRegion(spotlightBounds(spotlight).toAlignedRect(),
                          m_apertureShape == ApertureShape::Circle ? QRegion::Ellipse : QRegion::Rectangle);
    }
    return region;
}

void BlurWindow::addSpotlight()
{
    if (m_otherSpotlights.size() >= 15) {
        return;
    }
    m_otherSpotlights.append({m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize, m_spotlightZoom});
    m_spotlightZoom = 1.0;
    m_addButton->setEnabled(m_otherSpotlights.size() < 15);
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
            m_otherSpotlights[i] = {m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize, m_spotlightZoom};
            m_spotlightCenter = candidate.center;
            m_spotlightRadius = candidate.radius;
            m_spotlightRectSize = candidate.size;
            m_spotlightZoom = candidate.zoom;
            return;
        }
    }
}

void BlurWindow::deleteSpotlight()
{
    if (m_otherSpotlights.isEmpty()) {
        close();
        return;
    }
    const Spotlight next = m_otherSpotlights.takeLast();
    m_spotlightCenter = next.center;
    m_spotlightRadius = next.radius;
    m_spotlightRectSize = next.size;
    m_spotlightZoom = next.zoom;
    m_addButton->setEnabled(true);
    applyBlur();
    updateInputShape();
    update();
}

void BlurWindow::publishZoomRegions()
{
    QJsonArray regions;
    auto append = [this, &regions](const Spotlight &spotlight) {
        if (spotlight.zoom <= 1.0) {
            return;
        }
        const QRectF bounds = spotlightBounds(spotlight);
        regions.append(QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height(), spotlight.zoom});
    };
    if (isSpotlightMode() && isVisible() && !m_spotlightInverted) {
        for (const Spotlight &spotlight : m_otherSpotlights) {
            append(spotlight);
        }
        append({m_spotlightCenter, m_spotlightRadius, m_spotlightRectSize, m_spotlightZoom});
    }
    const QJsonObject payload{{QStringLiteral("regions"), regions},
                              {QStringLiteral("circle"), m_apertureShape == ApertureShape::Circle},
                              {QStringLiteral("blurred"), m_spotlightInverted}};
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

    const int xMargin = m_apertureShape == ApertureShape::Circle ? m_spotlightRadius : m_spotlightRectSize.width() / 2;
    const int yMargin = m_apertureShape == ApertureShape::Circle ? m_spotlightRadius : m_spotlightRectSize.height() / 2;
    m_spotlightCenter.setX(qBound(xMargin, m_spotlightCenter.x(), qMax(xMargin, width() - xMargin)));
    m_spotlightCenter.setY(qBound(yMargin, m_spotlightCenter.y(), qMax(yMargin, height() - yMargin)));
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

void BlurWindow::drawShapeButtons(QPainter *painter)
{
    auto drawButtonFrame = [painter](const QRect &rect, bool active, bool hovered, bool pressed) {
        const QColor background = active ? QColor(61, 174, 233, 95)
                                  : hovered ? QColor(245, 245, 245, 45)
                                            : QColor(20, 20, 20, 120);
        const QColor border = active ? QColor(61, 174, 233, 230) : QColor(245, 245, 245, 120);
        const QColor icon = active ? QColor(255, 255, 255, 240) : QColor(245, 245, 245, 190);

        QPainterPath path;
        path.addRoundedRect(QRectF(rect), 6, 6);
        painter->fillPath(path, background.darker(pressed ? 135 : 100));
        painter->setPen(QPen(border, active ? 1.8 : 1.2));
        painter->drawPath(path);
        return icon;
    };

    auto drawShapeButton = [this, painter, drawButtonFrame](const QRect &rect, ApertureShape shape, bool hovered, bool pressed) {
        const bool active = m_apertureShape == shape;
        const QColor icon = drawButtonFrame(rect, active, hovered, pressed);

        painter->setPen(QPen(icon, 2.0));
        const QRectF iconRect = QRectF(rect).adjusted(10, 10, -10, -10);
        if (shape == ApertureShape::Circle) {
            painter->drawEllipse(iconRect);
        } else {
            painter->drawRoundedRect(iconRect, 2, 2);
        }
    };

    drawShapeButton(circleShapeButtonRect(), ApertureShape::Circle, m_circleShapeHovered, m_circleShapePressed);
    drawShapeButton(rectangleShapeButtonRect(), ApertureShape::Rectangle, m_rectangleShapeHovered, m_rectangleShapePressed);

    const QRect invertRect = invertButtonRect();
    const QColor icon = drawButtonFrame(invertRect, m_spotlightInverted, m_invertHovered, m_invertPressed);
    const QRectF iconRect = QRectF(invertRect).adjusted(10, 10, -10, -10);
    QPainterPath leftHalf;
    leftHalf.moveTo(iconRect.topLeft());
    leftHalf.lineTo(iconRect.topRight());
    leftHalf.lineTo(iconRect.bottomLeft());
    leftHalf.closeSubpath();
    painter->fillPath(leftHalf, icon);
    painter->setPen(QPen(icon, 2.0));
    painter->drawRect(iconRect);
    painter->drawLine(iconRect.topRight(), iconRect.bottomLeft());
}
