#pragma once

#include <QImage>
#include <QList>
#include <QWidget>

namespace mixxx::b2b {

/// The flyer wall: on four decks, with the room's panels dragged down, the
/// space they leave shows the campaign's city posters in a row, like flyers
/// along a club's wall behind the booth. A different order each time the
/// app starts. Still while you play (nothing moves next to the decks), a
/// little dimmed so the decks stay first. A plain widget under the panels,
/// never over the decks' OpenGL views.
class FlyerWall : public QWidget {
  public:
    explicit FlyerWall(QWidget* pParent = nullptr);

    /// How tall the posters are (px): the whole wall, as it is when the
    /// panels are all the way down. The widget itself is only what's
    /// uncovered, from the top.
    void setPosterHeight(int px);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    struct Poster {
        QImage source;
        QImage scaled; // at the current height and screen
    };
    QList<Poster> m_posters;
    int m_posterHeight = 0;
    qreal m_scaledDpr = 0;
};

} // namespace mixxx::b2b
