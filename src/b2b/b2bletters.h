#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <memory>

class QPainter;
class QRectF;

namespace b2b {

// The lettering printed on a skin's picture, drawn by the app instead of
// inside the picture (the skins kit writes it beside the picture as
// <picture>.letters.json). Drawn at the size the picture is shown, on whole
// device pixels: a whole-pixel font size, each line's baseline and edge on a
// pixel. The same word comes out the same everywhere and stays steady and
// sharp at the small sizes four decks give it, where the same text scaled
// inside the picture lands between pixels and blurs.
class Letters {
  public:
    struct Line {
        QString text;
        double x = 0;       // its anchor (layout px of the picture)
        double y = 0;       // its baseline
        double size = 0;    // font size (em, layout px)
        int anchor = 0;     // -1 start, 0 middle, 1 end
        double squeeze = 1; // narrowed across (1: as the face is)
        QColor colour;
    };
    // A printed line (a fader's scale): drawn as a block of whole pixels,
    // at least one tall, so every line comes out alike.
    struct Rule {
        double x = 0; // layout px of the picture
        double y = 0; // its top
        double w = 0;
        double h = 0;
        QColor colour;
    };

    // The lettering for a picture file (its .letters.json beside it), or
    // nullptr if it has none. Loaded once per file.
    static std::shared_ptr<const Letters> forPicture(const QString& picturePath);

    // Draws it into `target` (where the whole picture is drawn, in the
    // painter's coordinates), scaled with the picture.
    void draw(QPainter* pPainter, const QRectF& target) const;

  private:
    double m_width = 0;  // the picture's size (layout px)
    double m_height = 0;
    QString m_face;
    int m_weight = 500;
    QList<Line> m_lines;
    QList<Rule> m_rules;
};

} // namespace b2b
