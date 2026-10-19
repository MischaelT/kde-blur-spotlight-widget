#pragma once

#include <QObject>
#include <QRect>
#include <QRectF>
#include <QString>

// Follows a region of the screen. Boxes are in logical pixels of the named
// screen, which equal widget coordinates of a fullscreen window on it.
class Tracker : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    virtual void start(const QRect &initialBox, const QString &screenName) = 0;
    virtual void stop() = 0;
    // Also try the chosen object at 80%-120% of its size when searching for it.
    virtual void setAutoSize(bool) {}

signals:
    void targetUpdated(const QRectF &box);
    void lost();
};
