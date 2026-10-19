#include "blurwindow.h"
#include "tracker.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QTemporaryDir>
#include <QWindow>
#include <QTimer>

#include <vector>

namespace
{
class FakeTracker : public Tracker
{
public:
    void start(const QRect &, const QString &) override {}
    void stop() override { stopped = true; }
    bool stopped = false;
};
}

int main(int argc, char *argv[])
{
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--self-test") == 0) {
            selfTest = true;
            break;
        }
    }
    if (selfTest && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QTemporaryDir testSettings;
    if (selfTest) {
        qputenv("XDG_CONFIG_HOME", testSettings.path().toUtf8());
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("local"));
    QCoreApplication::setApplicationName(QStringLiteral("blur-rectangle"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    BlurWindow window;
    if (selfTest) {
        window.resize(360, 130);
        window.show();
        QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &space);
        if (!window.isVisible()) {
            return 1;
        }
        window.resize(1000, 700);
        window.showDefaultMode();
        QCoreApplication::processEvents();
        window.resize(1000, 700);
        auto mouse = [&window](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(point), QPointF(window.mapToGlobal(point)), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &event);
        };
        auto pick = [&](QPoint point, const QString &text) {
            bool found = false;
            QTimer::singleShot(0, &window, [&found, text] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) {
                    return;
                }
                for (QAction *action : menu->actions()) {
                    if (action->text() == text) {
                        found = true;
                        menu->setActiveAction(action);
                        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                        QCoreApplication::sendEvent(menu, &enter);
                        return;
                    }
                }
                menu->close();
            });
            mouse(QEvent::MouseButtonPress, point, Qt::RightButton, Qt::RightButton);
            return found;
        };
        const QRegion original = window.windowHandle()->mask();
        if (!pick({420, 280}, QStringLiteral("New spotlight"))) {
            return 2;
        }
        if (!(original - window.windowHandle()->mask()).isEmpty()
            || window.windowHandle()->mask() == original) {
            return 3;
        }
        mouse(QEvent::MouseButtonPress, {620, 340}, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, {700, 450}, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {700, 450}, Qt::LeftButton, Qt::NoButton);
        if (!window.windowHandle()->mask().contains({700, 450})
            || !window.windowHandle()->mask().contains({420, 280})
            || window.windowHandle()->mask().contains({620, 270})) {
            return 4;
        }
        mouse(QEvent::MouseButtonPress, {798, 450}, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, {830, 450}, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {830, 450}, Qt::LeftButton, Qt::NoButton);
        if (!window.windowHandle()->mask().contains({835, 450})
            || !window.windowHandle()->mask().contains({420, 280})) {
            return 5;
        }
        if (!pick({700, 450}, QStringLiteral("Invert"))) {
            return 6;
        }
        if (!window.windowHandle()->mask().contains({420, 280})
            || !window.windowHandle()->mask().contains({700, 450})) {
            return 6;
        }
        const bool deleteFound = pick({700, 450}, QStringLiteral("Delete spotlight"));
        if (!deleteFound || window.windowHandle()->mask().contains({700, 450})
            || !window.windowHandle()->mask().contains({420, 280})) {
            return 7;
        }
        std::vector<FakeTracker *> fakes;
        window.setTrackerFactory([&fakes] {
            fakes.push_back(new FakeTracker);
            return fakes.back();
        });
        auto trackAt = [&](QPoint point) { pick(point, QStringLiteral("Track object")); };
        auto follow = [&](FakeTracker *fake, QPointF center) {
            emit fake->targetUpdated(QRectF(center.x() - 50, center.y() - 50, 100, 100));
            QElapsedTimer settle;
            settle.start();
            while (settle.elapsed() < 800) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
        };
        trackAt({420, 280});
        if (fakes.size() != 1) {
            return 8;
        }
        follow(fakes[0], {300, 500});
        if (!window.windowHandle()->mask().contains({300, 500})
            || window.windowHandle()->mask().contains({420, 280})) {
            return 9;
        }
        const QRegion beforeAdd = window.windowHandle()->mask();
        pick({420, 280}, QStringLiteral("New spotlight"));
        const QPoint second = (window.windowHandle()->mask() - beforeAdd).boundingRect().center();
        trackAt(second);
        if (fakes.size() != 2) {
            return 11;
        }
        follow(fakes[0], {700, 150});
        if (!window.windowHandle()->mask().contains({700, 150})
            || !window.windowHandle()->mask().contains(second)) {
            return 12;
        }
        follow(fakes[1], {150, 150});
        if (!window.windowHandle()->mask().contains({150, 150})
            || !window.windowHandle()->mask().contains({700, 150})
            || window.windowHandle()->mask().contains(second)) {
            return 13;
        }
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &escape);
        if (!fakes[0]->stopped || !fakes[1]->stopped || !window.isVisible()) {
            return 10;
        }
        // Spotlight cap: a third spotlight is allowed, a fourth is not.
        auto key = [&window](int code) {
            QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &event);
        };
        const QRegion beforeThird = window.windowHandle()->mask();
        pick({420, 280}, QStringLiteral("New spotlight"));
        const QRegion atCap = window.windowHandle()->mask();
        key(Qt::Key_N);
        if (window.windowHandle()->mask() != atCap) {
            return 15;
        }
        // Autotrack after a normal drag: drag, let go, stay still, the dragged spotlight follows its object.
        key(Qt::Key_A);
        const QPoint dragFrom = (atCap - beforeThird).boundingRect().center();
        const QPoint dragTo = dragFrom + QPoint(30, 0);
        const size_t trackersBefore = fakes.size();
        mouse(QEvent::MouseButtonPress, dragFrom, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, dragTo, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, dragTo, Qt::LeftButton, Qt::NoButton);
        QElapsedTimer countdown;
        countdown.start();
        while (countdown.elapsed() < 3600) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        if (fakes.size() != trackersBefore + 1) {
            return 16;
        }
        follow(fakes.back(), {900, 600});
        if (!window.windowHandle()->mask().contains({900, 600})) {
            return 17;
        }
        key(Qt::Key_A);
        // Freeform: press F, draw a triangle, the spotlight takes its place and shape.
        QKeyEvent freeform(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &freeform);
        const QPoint corners[] = {{520, 420}, {660, 420}, {590, 300}, {520, 420}};
        mouse(QEvent::MouseButtonPress, corners[0], Qt::LeftButton, Qt::LeftButton);
        for (int i = 0; i < 3; ++i) {
            for (int step = 1; step <= 10; ++step) {
                const QPoint point = corners[i] + (corners[i + 1] - corners[i]) * step / 10;
                mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
            }
        }
        mouse(QEvent::MouseButtonRelease, corners[0], Qt::LeftButton, Qt::NoButton);
        if (!window.windowHandle()->mask().contains({590, 380})) {
            return 14;
        }
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        return app.exec();
    }

    window.showDefaultMode();
    return app.exec();
}
