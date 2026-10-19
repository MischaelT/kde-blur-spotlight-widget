#include "blurwindow.h"

#include "blurwindow_constants.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDBusInterface>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>

#include <algorithm>

using namespace blurwindow;

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

    QAction *spotlightModeAction = menu->addAction(QStringLiteral("Spotlight blocker mode"));
    connect(spotlightModeAction, &QAction::triggered, this, [this] {
        setMode(Mode::Spotlight);
    });
    connect(menu, &QMenu::aboutToShow, this, [this, spotlightModeAction] {
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
    trayIcon->setToolTip(QStringLiteral("Blur Spotlight"));
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

void BlurWindow::showContextMenu(const QPoint &globalPosition)
{
    QMenu menu(this);
    QAction *deleteAction = nullptr;
    QAction *trackAction = nullptr;
    QAction *newAction = nullptr;
    QAction *autoTrackAction = nullptr;
    QAction *autoSizeAction = nullptr;
    QAction *invertAction = nullptr;
    QAction *circleAction = nullptr;
    QAction *rectangleAction = nullptr;
    QAction *freeformAction = nullptr;
    QActionGroup zoomActions(&menu);
    zoomActions.setExclusive(true);
    const QPoint local = mapFromGlobal(globalPosition);
    if (isSpotlightMode() && (isInsideAperture(local) || isOnSpotlightBorder(local))) {
        deleteAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Delete spotlight"));
        deleteAction->setShortcut(Qt::Key_Delete);
        QMenu *zoomMenu = menu.addMenu(QIcon::fromTheme(QStringLiteral("zoom-in")), QStringLiteral("Zoom"));
        zoomMenu->menuAction()->setShortcut(Qt::Key_Z);
        for (double factor : kZoomFactors) {
            QAction *action = zoomMenu->addAction(factor == 1.0 ? QStringLiteral("Off (1x)")
                                                              : QStringLiteral("%1x").arg(factor));
            action->setCheckable(true);
            action->setData(factor);
            action->setChecked(qFuzzyCompare(m_spotlightZoom, factor));
            zoomActions.addAction(action);
        }
        const QDBusInterface zoomEffect(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/local/StableBlur"),
                                        QStringLiteral("org.local.StableBlur"));
        zoomMenu->setEnabled(zoomEffect.isValid() && !m_spotlightInverted && m_apertureShape != ApertureShape::Freeform);
        if (m_spotlightInverted) {
            zoomMenu->menuAction()->setToolTip(QStringLiteral("Zoom is available when the spotlight is clear"));
            menu.setToolTipsVisible(true);
        }
        if (!zoomEffect.isValid()) {
            zoomMenu->menuAction()->setToolTip(QStringLiteral("Install and reload the updated Stable Blur effect to enable zoom"));
            menu.setToolTipsVisible(true);
        }
        trackAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-find")),
                                     m_tracker ? QStringLiteral("Stop tracking") : QStringLiteral("Track object"));
        trackAction->setShortcut(Qt::Key_T);
        menu.addSeparator();
    }
    if (isSpotlightMode()) {
        newAction = menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("New spotlight"));
        newAction->setShortcut(Qt::Key_N);
        newAction->setEnabled(m_otherSpotlights.size() < kMaxSpotlights - 1);
        invertAction = menu.addAction(QStringLiteral("Invert"));
        invertAction->setShortcut(Qt::Key_I);
        invertAction->setCheckable(true);
        invertAction->setChecked(m_spotlightInverted);
        autoTrackAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-find")), QStringLiteral("Autotrack"));
        autoTrackAction->setShortcut(Qt::Key_A);
        autoTrackAction->setCheckable(true);
        autoTrackAction->setChecked(m_autoTrack);
        autoSizeAction = menu.addAction(QStringLiteral("Autosize"));
        autoSizeAction->setShortcut(Qt::Key_W);
        autoSizeAction->setCheckable(true);
        autoSizeAction->setChecked(m_autoSize);
        QMenu *shapeMenu = menu.addMenu(QStringLiteral("Shape"));
        circleAction = shapeMenu->addAction(QStringLiteral("Circle"));
        circleAction->setShortcut(Qt::Key_C);
        circleAction->setCheckable(true);
        circleAction->setChecked(m_apertureShape == ApertureShape::Circle);
        rectangleAction = shapeMenu->addAction(QStringLiteral("Rectangle"));
        rectangleAction->setShortcut(Qt::Key_R);
        rectangleAction->setCheckable(true);
        rectangleAction->setChecked(m_apertureShape == ApertureShape::Rectangle);
        freeformAction = shapeMenu->addAction(QStringLiteral("Freeform"));
        freeformAction->setShortcut(Qt::Key_F);
        freeformAction->setCheckable(true);
        freeformAction->setChecked(m_apertureShape == ApertureShape::Freeform);
        menu.addSeparator();
    }
    QAction *hideAction = menu.addAction(QStringLiteral("Hide"));
    hideAction->setShortcut(Qt::Key_H);
    QAction *spotlightModeAction = menu.addAction(QStringLiteral("Spotlight blocker mode"));
    spotlightModeAction->setVisible(!isSpotlightMode());
    spotlightModeAction->setShortcut(Qt::Key_S);
    QAction *clickThroughAction = menu.addAction(QStringLiteral("Click-through"));
    clickThroughAction->setShortcut(Qt::CTRL | Qt::Key_T);
    clickThroughAction->setCheckable(true);
    clickThroughAction->setChecked(m_clickThrough);
    clickThroughAction->setEnabled(!isSpotlightMode());
    menu.addSeparator();
    QAction *quitAction = menu.addAction(QStringLiteral("Quit"));
    quitAction->setShortcut(Qt::CTRL | Qt::Key_Q);

    QAction *chosen = menu.exec(globalPosition);
    if (!chosen) {
        return;
    }
    if (chosen == deleteAction) {
        deleteSpotlight();
    } else if (chosen == newAction) {
        addSpotlight();
    } else if (chosen == autoTrackAction) {
        m_autoTrack = autoTrackAction->isChecked();
        if (!m_autoTrack) {
            cancelAutoTrack();
        } else if (m_followingMouse) {
            armAutoTrack();
        }
        saveSettings();
    } else if (chosen == autoSizeAction) {
        setAutoSize(autoSizeAction->isChecked());
    } else if (chosen == invertAction) {
        setSpotlightInverted(!m_spotlightInverted);
    } else if (chosen == circleAction) {
        setApertureShape(ApertureShape::Circle);
    } else if (chosen == rectangleAction) {
        setApertureShape(ApertureShape::Rectangle);
    } else if (chosen == freeformAction) {
        startDrawingOutline();
    } else if (chosen == trackAction) {
        if (m_tracker) {
            stopTracking();
        } else {
            startTracking();
        }
    } else if (zoomActions.actions().contains(chosen)) {
        m_spotlightZoom = chosen->data().toDouble();
        QTimer::singleShot(0, this, &BlurWindow::applyBlur);
    } else if (chosen == hideAction) {
        hide();
    } else if (chosen == spotlightModeAction) {
        setMode(Mode::Spotlight);
    } else if (chosen == clickThroughAction) {
        setClickThrough(clickThroughAction->isChecked());
    } else if (chosen == quitAction) {
        qApp->quit();
    }
}

void BlurWindow::cycleZoom()
{
    const QDBusInterface zoomEffect(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/local/StableBlur"),
                                    QStringLiteral("org.local.StableBlur"));
    if (!zoomEffect.isValid() || m_spotlightInverted || m_apertureShape == ApertureShape::Freeform) {
        return;
    }
    const auto *current = std::find_if(std::begin(kZoomFactors), std::end(kZoomFactors),
                                       [this](double factor) { return qFuzzyCompare(m_spotlightZoom, factor); });
    const auto *next = current == std::end(kZoomFactors) ? current : current + 1;
    m_spotlightZoom = next == std::end(kZoomFactors) ? kZoomFactors[0] : *next;
    applyBlur();
}
