#include "blurwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWindow>
#include <QTimer>

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
        window.resize(1000, 700);
        auto *add = window.findChild<QToolButton *>();
        if (!add || !add->isVisible()) {
            return 2;
        }
        const QRegion original = window.windowHandle()->mask();
        add->click();
        if (!(original - window.windowHandle()->mask()).isEmpty()
            || window.windowHandle()->mask() == original) {
            return 3;
        }
        auto mouse = [&window](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(point), QPointF(window.mapToGlobal(point)), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &event);
        };
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
        mouse(QEvent::MouseButtonPress, {79, 667}, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {79, 667}, Qt::LeftButton, Qt::NoButton);
        if (!window.windowHandle()->mask().contains({575, 190})
            || !window.windowHandle()->mask().contains({805, 510})) {
            return 6;
        }
        bool deleteFound = false;
        QTimer::singleShot(0, &window, [&deleteFound] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) {
                return;
            }
            for (QAction *action : menu->actions()) {
                if (action->text() == QStringLiteral("Delete spotlight")) {
                    deleteFound = true;
                    menu->setActiveAction(action);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QCoreApplication::sendEvent(menu, &enter);
                    return;
                }
            }
            menu->close();
        });
        mouse(QEvent::MouseButtonPress, {700, 450}, Qt::RightButton, Qt::RightButton);
        if (!deleteFound || window.windowHandle()->mask().contains({700, 450})
            || !window.windowHandle()->mask().contains({420, 280})) {
            return 7;
        }
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        return app.exec();
    }

    window.showDefaultMode();
    return app.exec();
}
