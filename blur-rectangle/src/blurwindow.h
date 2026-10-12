#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QSettings>
#include <QWidget>
#include <QVector>

class QMenu;
class QSystemTrayIcon;
class QToolButton;

class BlurWindow : public QWidget
{
    Q_OBJECT

public:
    explicit BlurWindow(QWidget *parent = nullptr);
    ~BlurWindow() override;
    void showDefaultMode();

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
    };
    void addSpotlight();
    void deleteSpotlight();
    void publishZoomRegions();
    double m_spotlightZoom = 1.0;
    void selectSpotlight(const QPoint &position);
    QRectF spotlightBounds(const Spotlight &spotlight) const;
    QRegion allAperturesRegion() const;
    QVector<Spotlight> m_otherSpotlights;
    QToolButton *m_addButton = nullptr;
    void applyBlur();
    void createTrayIcon();
    void loadSettings();
    void saveSettings();
    void setBlurLevel(int level);
    void setClickThrough(bool enabled);
    void setInteractiveState(bool active);
    void setMode(Mode mode);
    void setApertureShape(ApertureShape shape);
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
    QRect circleShapeButtonRect() const;
    QRect rectangleShapeButtonRect() const;
    QRect invertButtonRect() const;
    QRect handleRect() const;
    QRectF spotlightRect() const;
    QRectF spotlightRectangleRect() const;
    bool isSpotlightMode() const;
    bool isOnSpotlightBorder(const QPointF &position) const;
    bool isInsideAperture(const QPointF &position) const;
    void clampSpotlightCenter();
    void drawTitleButtons(QPainter *painter);
    void drawShapeButtons(QPainter *painter);

    QSettings m_settings;
    QPointer<QSystemTrayIcon> m_trayIcon;
    QPointer<QMenu> m_trayMenu;
    QColor m_tintColor;
    int m_tintAlpha = 1;
    int m_borderAlpha = 210;
    int m_blurLevel = 4;
    Mode m_mode = Mode::BlurRectangle;
    ApertureShape m_apertureShape = ApertureShape::Circle;
    bool m_hovered = false;
    bool m_interacting = false;
    bool m_clickThrough = false;
    bool m_blurAvailable = false;
    bool m_draggingSpotlight = false;
    bool m_resizingSpotlight = false;
    bool m_followingMouse = false;
    bool m_minimizeHovered = false;
    bool m_minimizePressed = false;
    bool m_modeButtonHovered = false;
    bool m_modeButtonPressed = false;
    bool m_closeHovered = false;
    bool m_closePressed = false;
    bool m_circleShapeHovered = false;
    bool m_circleShapePressed = false;
    bool m_rectangleShapeHovered = false;
    bool m_rectangleShapePressed = false;
    bool m_invertHovered = false;
    bool m_invertPressed = false;
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
