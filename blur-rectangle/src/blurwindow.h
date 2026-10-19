#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <functional>
#include <QPointer>
#include <QSettings>
#include <QWidget>
#include <QVector>

class QMenu;
class QSystemTrayIcon;
class QTimer;
class Tracker;

class BlurWindow : public QWidget
{
    Q_OBJECT

public:
    explicit BlurWindow(QWidget *parent = nullptr);
    ~BlurWindow() override;
    void showDefaultMode();
    void setTrackerFactory(std::function<Tracker *()> factory);

    enum Edge {
        NoEdge = 0,
        LeftEdge = 1 << 0,
        TopEdge = 1 << 1,
        RightEdge = 1 << 2,
        BottomEdge = 1 << 3,
    };
    Q_DECLARE_FLAGS(Edges, Edge)

    enum class Mode {
        BlurRectangle,
        Spotlight,
    };

    enum class ApertureShape {
        Circle,
        Rectangle,
        Freeform, // a hand-drawn outline, scaled into the rectangle box
    };

protected:
    void closeEvent(QCloseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Spotlight {
        QPoint center;
        int radius;
        QSize size;
        double zoom = 1.0;
        Tracker *tracker = nullptr;
    };
    void addSpotlight();
    void deleteSpotlight();
    void startTracking();
    void cycleZoom();
    void armAutoTrack();
    void cancelAutoTrack();
    void beginPendingTracking();
    Tracker *launchTracker();
    void registerTracking(Tracker *tracker);
    void setAutoSize(bool enabled);
    void stopTracking();
    void stopTracker(Tracker *tracker);
    void stopAllTracking();
    void onTargetUpdated(Tracker *tracker, const QRectF &box);
    void stepTracking();
    QPoint clampedCenter(QPoint center, int radius, const QSize &size) const;
    void publishZoomRegions();
    double m_spotlightZoom = 1.0;
    void selectSpotlight(const QPoint &position);
    QRectF spotlightBounds(const Spotlight &spotlight) const;
    QRegion allAperturesRegion() const;
    QVector<Spotlight> m_otherSpotlights;
    void applyBlur();
    void createTrayIcon();
    void loadSettings();
    void saveSettings();
    void setBlurLevel(int level);
    void setClickThrough(bool enabled);
    void setInteractiveState(bool active);
    void setMode(Mode mode);
    void setApertureShape(ApertureShape shape);
    void startDrawingOutline();
    void finishOutline();
    void cancelOutline();
    QPainterPath aperturePath(const QRectF &bounds) const;
    QRegion apertureRegionIn(const QRectF &bounds) const;
    void setSpotlightInverted(bool inverted);
    void setSpotlightRadius(int radius);
    void setSpotlightRectSize(const QSize &size);
    void showContextMenu(const QPoint &globalPosition);
    void updateCursor(const QPoint &position);
    void updateInputShape();

    Edges edgesAt(const QPoint &position) const;
    Edges spotlightRectEdgesAt(const QPoint &position) const;
    QRegion apertureRegion() const;
    QRegion spotlightInputRegion() const;
    QRect minimizeButtonRect() const;
    QRect modeButtonRect() const;
    QRect closeButtonRect() const;
    QRect handleRect() const;
    QRectF spotlightRect() const;
    QRectF spotlightRectangleRect() const;
    bool isSpotlightMode() const;
    bool isOnSpotlightBorder(const QPointF &position) const;
    bool isInsideAperture(const QPointF &position) const;
    void clampSpotlightCenter();
    void drawTitleButtons(QPainter *painter);

    QSettings m_settings;
    QPointer<QSystemTrayIcon> m_trayIcon;
    QPointer<QMenu> m_trayMenu;
    QColor m_tintColor;
    int m_tintAlpha = 1;
    int m_borderAlpha = 210;
    int m_blurLevel = 4;
    Mode m_mode = Mode::BlurRectangle;
    ApertureShape m_apertureShape = ApertureShape::Circle;
    QPolygonF m_outline;        // freeform outline, points in 0..1 of the spotlight box
    QPolygonF m_stroke;         // outline being drawn, in widget coordinates
    bool m_drawingOutline = false;
    bool m_hovered = false;
    bool m_interacting = false;
    bool m_clickThrough = false;
    bool m_blurAvailable = false;
    bool m_draggingSpotlight = false;
    bool m_resizingSpotlight = false;
    bool m_followingMouse = false;
    bool m_dragAutoTrack = false; // autotrack countdown started by a normal drag, still running after release
    Tracker *m_tracker = nullptr;
    struct TrackState {
        QPointF position;
        QPointF target;
        QPointF velocity;
        double baseWidth = 0;   // box width when tracking started
        int baseRadius = 0;
        QSize baseSize;
    };
    QHash<Tracker *, TrackState> m_trackStates;
    QTimer *m_trackTimer = nullptr;
    QTimer *m_idleTimer = nullptr;
    QTimer *m_dotsTimer = nullptr;
    QTimer *m_settleTimer = nullptr;
    Tracker *m_pendingTracker = nullptr; // memorized when the pointer settled, followed once the countdown ends
    QTimer *m_followMoveTimer = nullptr;
    bool m_followMoving = false; // pointer moved lately while following: input area is widened
    bool m_autoTrack = false;
    bool m_autoSize = false;
    QElapsedTimer m_trackClock;
    std::function<Tracker *()> m_trackerFactory;
    bool m_minimizeHovered = false;
    bool m_minimizePressed = false;
    bool m_modeButtonHovered = false;
    bool m_modeButtonPressed = false;
    bool m_closeHovered = false;
    bool m_closePressed = false;
    bool m_spotlightInverted = false;
    Edges m_pressedEdges = NoEdge;
    Edges m_spotlightResizeEdges = NoEdge;
    QPoint m_pressLocalPosition;
    QSize m_pressSize;
    QRect m_normalGeometry;
    QPoint m_spotlightCenter;
    QPoint m_spotlightDragOffset;
    QPoint m_spotlightResizeStartPosition;
    QPoint m_spotlightResizeStartCenter;
    QSize m_spotlightResizeStartSize;
    int m_spotlightRadius = 140;
    QSize m_spotlightRectSize = QSize(320, 190);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(BlurWindow::Edges)
