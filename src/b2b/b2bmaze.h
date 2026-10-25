#pragma once

#include <QElapsedTimer>
#include <QIcon>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

class QPainter;

namespace mixxx::b2b {

/// PitchRide's maze: the logo and the loading animation (DESIGN.md 4,
/// LOCKED). Drawn here, sharp at any size, from res/b2b/brand/
/// logo-maze.json (seed 150: the rings, the open passages, the route, the
/// timings, the colours), the way res/b2b/brand/maze.py draws logo.svg.
/// Never regenerated or changed here: the json is the maze.
namespace maze {

/// How to draw it.
struct Look {
    /// The orange route from the gap to the centre (the logo). Off: the
    /// walls and the flower only (logo-walls.svg: small sizes).
    bool route = true;
    /// How much of the route is drawn, from the gap (0) to its end inside
    /// a petal (1).
    double drawn = 1.0;
    /// The flower's size (0: not there; 1: as in the logo; a little more
    /// at the top of its bloom).
    double bloom = 1.0;
    double flowerOpacity = 1.0;
};

/// Draws the maze centred in `box` (the logo's own margin included; the
/// route starts just below it, as in the SVGs).
void paint(QPainter& p, const QRectF& box, const Look& look = Look());

/// The loading animation `ms` after it started (loading.html, with
/// logo-maze.json's timings): the route draws from the gap to the centre,
/// then the flower blooms; then it starts again from the gap (a loop of
/// loopMs(), never running backwards).
Look loading(qint64 ms);
int loopMs();

/// The daisy alone (a friend's room), `radius` to its petals' tips.
void paintFlower(QPainter& p, const QPointF& centre, double radius);

/// The logo on its own (transparent round it); walls only under 24 px.
QPixmap picture(int size, qreal dpr);
/// The window's icon: the logo on the ground blue, at every size a
/// window, a task bar or a dock asks for (walls only under 24 px).
QIcon appIcon();

} // namespace maze

/// The logo as a widget (the top bar, the Rooms page): `size` square, the
/// route left out under 24 px, or always (`route` false: logo-walls, as the
/// pages show it).
class MazeLogo : public QWidget {
  public:
    MazeLogo(int size, QWidget* pParent = nullptr, bool route = true);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    bool m_route;
};

/// Waiting for something: the loading maze, looping while it's shown, and
/// a typed line under it (a progress report). A plain widget that paints
/// itself, so it can go over anything: no native window, no graphics
/// effect (it fades in by painting its own opacity, as the toast does).
class MazeWait : public QWidget {
  public:
    explicit MazeWait(QWidget* pParent = nullptr);

    /// The note under the maze (lowercase, typed). Empty: none.
    void setText(const QString& text);
    /// Paints the page's blue under it (on its own over something); off:
    /// see-through (on a page that paints its own).
    void setOpaque(bool opaque);
    /// From the gap again (a new wait).
    void restart();

    /// A wait painted into `rect` with `p`, `ms` into it (for widgets that
    /// paint their own waiting state, like the room's booth).
    static void paintWait(QPainter& p, const QRectF& rect, qint64 ms, const QString& text);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;

  private:
    QString m_text;
    bool m_opaque = true;
    QElapsedTimer m_clock;
    QTimer m_frame;
};

} // namespace mixxx::b2b
