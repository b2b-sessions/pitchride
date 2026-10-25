#include "b2b/b2bmaze.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QtDebug>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <vector>

#include "b2b/b2btheme.h"

namespace mixxx::b2b {

namespace maze {

namespace {

/// A cell: ring r (0 innermost), cell k round it; r < 0: the centre.
struct Cell {
    int r = -1;
    int k = 0;
    bool centre() const {
        return r < 0;
    }
};

QString key(const Cell& c) {
    return c.centre() ? QStringLiteral("centre") : QStringLiteral("%1,%2").arg(c.r).arg(c.k);
}

QString edge(const Cell& a, const Cell& b) {
    const QString ka = key(a);
    const QString kb = key(b);
    return ka < kb ? ka + QLatin1Char('|') + kb : kb + QLatin1Char('|') + ka;
}

Cell cellOf(const QJsonValue& v) {
    if (v.isString()) {
        return Cell(); // "centre"
    }
    const QJsonArray a = v.toArray();
    return Cell{a.at(0).toInt(), a.at(1).toInt()};
}

/// A line of the drawing, in maze units round the centre: polar points.
using Polyline = std::vector<QPointF>; // (radius, angle)

/// logo-maze.json, and what maze.py works out from it, once.
struct Geometry {
    std::vector<int> rings;
    double rc = 1.6;
    double w = 1.0;
    double rout = 0;
    QSet<QString> open;
    std::vector<Cell> path;
    int loop = 3600;
    int lineDone = 2230;
    int bloomPeak = 2590;
    QColor walls = QColor(0xE3, 0xA0, 0x6B);
    QColor line = QColor(0xF2, 0x51, 0x1E);
    QColor flowerCentre = QColor(0xF7, 0xD6, 0xDC);
    QColor ground = QColor(0x11, 0x22, 0x83);
    // Drawn from the above:
    std::vector<Polyline> wallLines;
    Polyline route; // from just outside the gap to its end inside a petal
    double endAngle = 0;
    bool ok = false;

    int n(int r) const {
        return rings[r];
    }
    int count() const {
        return static_cast<int>(rings.size());
    }
    // maze.py: off, span, mid, rin, parent.
    double off() const {
        return M_PI / 2 - M_PI / rings.back(); // outer cell 0 centred on the bottom
    }
    double spanA(int r, int k) const {
        return off() + k * 2 * M_PI / n(r);
    }
    double spanB(int r, int k) const {
        return spanA(r, k) + 2 * M_PI / n(r);
    }
    double mid(const Cell& c) const {
        return (spanA(c.r, c.k) + spanB(c.r, c.k)) / 2;
    }
    double rin(int r) const {
        return rc + r * w;
    }
    double rmid(int r) const {
        return rin(r) + w / 2;
    }
    Cell parent(const Cell& c) const {
        return Cell{c.r - 1, c.k * n(c.r - 1) / n(c.r)};
    }
    bool connected(const Cell& a, const Cell& b) const {
        return open.contains(edge(a, b));
    }
};

/// Python's (a % b) for a positive b: never negative.
double pyMod(double a, double b) {
    const double m = std::fmod(a, b);
    return m < 0 ? m + b : m;
}

/// maze.py's sdelta: the signed turn from a to b, the short way round.
double sdelta(double a, double b) {
    return pyMod(b - a + M_PI, 2 * M_PI) - M_PI;
}

/// An arc at radius r from angle a to b, as polar points (small steps:
/// smooth at any size).
void arc(Polyline* pLine, double r, double a, double b) {
    const int steps = std::max(2, static_cast<int>(std::ceil(std::abs(b - a) / qDegreesToRadians(1.0))));
    for (int i = 0; i <= steps; ++i) {
        pLine->push_back(QPointF(r, a + (b - a) * i / steps));
    }
}

Geometry load() {
    Geometry g;
    QFile file(QStringLiteral(":/b2b/brand/logo-maze.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "B2B maze: no logo-maze.json";
        return g;
    }
    const QJsonObject j = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue& v : j.value(QStringLiteral("rings")).toArray()) {
        g.rings.push_back(v.toInt());
    }
    if (g.rings.empty()) {
        qWarning() << "B2B maze: logo-maze.json has no rings";
        return g;
    }
    g.rc = j.value(QStringLiteral("rc")).toDouble(1.6);
    g.w = j.value(QStringLiteral("ring_width")).toDouble(1.0);
    g.rout = g.rc + g.count() * g.w;
    for (const QJsonValue& v : j.value(QStringLiteral("open")).toArray()) {
        const QJsonArray pair = v.toArray();
        g.open.insert(edge(cellOf(pair.at(0)), cellOf(pair.at(1))));
    }
    for (const QJsonValue& v : j.value(QStringLiteral("path")).toArray()) {
        g.path.push_back(cellOf(v));
    }
    const QJsonObject t = j.value(QStringLiteral("timing_ms")).toObject();
    g.loop = t.value(QStringLiteral("loop")).toInt(g.loop);
    g.lineDone = t.value(QStringLiteral("line_done")).toInt(g.lineDone);
    g.bloomPeak = t.value(QStringLiteral("bloom_peak")).toInt(g.bloomPeak);
    const QJsonObject c = j.value(QStringLiteral("colours")).toObject();
    g.ground = QColor(c.value(QStringLiteral("ground")).toString(g.ground.name()));
    g.walls = QColor(c.value(QStringLiteral("walls")).toString(g.walls.name()));
    g.line = QColor(c.value(QStringLiteral("line")).toString(g.line.name()));
    g.flowerCentre = QColor(c.value(QStringLiteral("flower_centre")).toString(g.flowerCentre.name()));

    // The walls (maze.py svg(): inner wall, radial wall on the far edge,
    // the outer wall but for the gap).
    const Cell entrance{g.count() - 1, 0};
    for (int r = 0; r < g.count(); ++r) {
        for (int k = 0; k < g.n(r); ++k) {
            const Cell cell{r, k};
            const double a = g.spanA(r, k);
            const double b = g.spanB(r, k);
            const bool innerOpen = r == 0 ? g.connected(cell, Cell()) : g.connected(cell, g.parent(cell));
            if (!innerOpen) {
                Polyline line;
                arc(&line, g.rin(r), a, b);
                g.wallLines.push_back(line);
            }
            if (!g.connected(cell, Cell{r, (k + 1) % g.n(r)})) {
                g.wallLines.push_back(Polyline{QPointF(g.rin(r), b), QPointF(g.rin(r) + g.w, b)});
            }
            if (r == g.count() - 1 && !(k == entrance.k)) {
                Polyline line;
                arc(&line, g.rout, a, b);
                g.wallLines.push_back(line);
            }
        }
    }

    // The route (maze.py path_ops), from just outside the gap. It stops
    // inside the petal that points back along it: 6.0 flower units out.
    const std::vector<Cell>& p = g.path;
    if (p.size() >= 2 && !p.front().centre()) {
        Polyline& route = g.route;
        route.push_back(QPointF(g.rout + 0.7, M_PI / 2));
        double ang = g.mid(p[0]);
        route.push_back(QPointF(g.rmid(p[0].r), ang));
        const double flowerScale = g.rc * 0.5 / 10; // fs / sc in maze.py
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            const Cell& A = p[i];
            const Cell& B = p[i + 1];
            if (B.centre()) {
                route.push_back(QPointF(6.0 * flowerScale, ang));
                g.endAngle = ang;
                break;
            }
            const double ra = g.rmid(A.r);
            const double rb = g.rmid(B.r);
            const double ab = g.mid(B);
            if (A.r == B.r) {
                arc(&route, ra, ang, ang + sdelta(ang, ab));
                ang = ab;
            } else if (B.r < A.r) {
                route.push_back(QPointF(rb, ang));
            } else {
                arc(&route, ra, ang, ang + sdelta(ang, ab));
                ang = ab;
                route.push_back(QPointF(rb, ang));
            }
        }
    }
    g.ok = !g.wallLines.empty() && g.route.size() > 2;
    if (!g.ok) {
        qWarning() << "B2B maze: logo-maze.json could not be drawn";
    }
    return g;
}

const Geometry& geometry() {
    static const Geometry g = load();
    return g;
}

/// The daisy (maze.py's DAISY): twelve petals and the centre, in flower
/// units, its first petal pointing up.
void daisy(QPainter& p, const QColor& petals, const QColor& centre) {
    p.setPen(Qt::NoPen);
    p.setBrush(petals);
    for (int i = 0; i < 12; ++i) {
        p.save();
        p.rotate(i * 30.0);
        p.drawEllipse(QPointF(0, -6.6), 1.4, 3.7);
        p.restore();
    }
    p.setBrush(centre);
    p.drawEllipse(QPointF(0, 0), 2.9, 2.9);
}

} // namespace

void paint(QPainter& p, const QRectF& box, const Look& look) {
    const Geometry& g = geometry();
    if (!g.ok) {
        return;
    }
    const double size = std::min(box.width(), box.height());
    const double sc = (size / 2 * 0.94) / g.rout; // maze.py: px per maze unit
    const QPointF centre = box.center();
    auto at = [&](const QPointF& polar) {
        return QPointF(centre.x() + polar.x() * sc * std::cos(polar.y()),
                centre.y() + polar.x() * sc * std::sin(polar.y()));
    };
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // The SVGs' line width, but never under about 0.6 of a device pixel (at icon
    // sizes it would wash out to nothing).
    const double device = std::max(1e-6, std::abs(p.deviceTransform().m11()));
    const double sw = std::max(0.15 * sc, 0.6 / device);
    QPainterPath walls;
    for (const Polyline& line : g.wallLines) {
        walls.moveTo(at(line.front()));
        for (size_t i = 1; i < line.size(); ++i) {
            walls.lineTo(at(line[i]));
        }
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(g.walls, sw, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(walls);

    if (look.route && look.drawn > 0) {
        // As far along it as `drawn` says (by length, as the SVG's dash).
        std::vector<QPointF> points;
        points.reserve(g.route.size());
        for (const QPointF& polar : g.route) {
            points.push_back(at(polar));
        }
        double total = 0;
        for (size_t i = 1; i < points.size(); ++i) {
            total += QLineF(points[i - 1], points[i]).length();
        }
        double left = total * std::min(1.0, look.drawn);
        QPainterPath route(points.front());
        for (size_t i = 1; i < points.size() && left > 0; ++i) {
            const QLineF step(points[i - 1], points[i]);
            if (step.length() <= left) {
                route.lineTo(points[i]);
                left -= step.length();
            } else {
                route.lineTo(step.pointAt(left / step.length()));
                left = 0;
            }
        }
        p.setPen(QPen(g.line, sw * 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(route);
    }

    if (look.bloom > 0 && look.flowerOpacity > 0) {
        // maze.py: translate to the centre, turn the first petal towards the
        // route's end, flower units (fs) scaled; blooming from its centre.
        const double fs = g.rc * 0.5 * sc / 10;
        p.save();
        p.setOpacity(p.opacity() * std::min(1.0, look.flowerOpacity));
        p.translate(centre);
        p.rotate(qRadiansToDegrees(g.endAngle) + 90);
        p.scale(fs * look.bloom, fs * look.bloom);
        daisy(p, g.line, g.flowerCentre);
        p.restore();
    }
    p.restore();
}

namespace {

/// CSS's cubic-bezier(x1, y1, x2, y2) timing at progress x (0..1).
double cubicBezier(double x1, double y1, double x2, double y2, double x) {
    if (x <= 0) {
        return 0;
    }
    if (x >= 1) {
        return 1;
    }
    auto bx = [&](double t) {
        return 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t;
    };
    auto by = [&](double t) {
        return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t;
    };
    double lo = 0;
    double hi = 1;
    for (int i = 0; i < 40; ++i) { // x(t) only rises: halve to the t that gives x
        const double mid = (lo + hi) / 2;
        (bx(mid) < x ? lo : hi) = mid;
    }
    return by((lo + hi) / 2);
}

double easeInOut(double x) {
    return cubicBezier(0.42, 0, 0.58, 1, x);
}

double easeOut(double x) {
    return cubicBezier(0, 0, 0.58, 1, x);
}

} // namespace

int loopMs() {
    return std::max(1, geometry().loop);
}

Look loading(qint64 ms) {
    // loading.html: the line draws 0% to 62% of the loop (ease-in-out);
    // the flower is hidden to 60%, at 1.18 at 72% and settles to 1 by 80%
    // (ease-out). logo-maze.json: loop, line_done (62%), bloom_peak (72%).
    const Geometry& g = geometry();
    const double loop = loopMs();
    const double t = static_cast<double>(ms % static_cast<qint64>(loop));
    const double bloomFrom = 0.60 * loop;
    const double settled = 0.80 * loop;
    Look look;
    look.drawn = t < g.lineDone ? easeInOut(t / g.lineDone) : 1.0;
    if (t < bloomFrom) {
        look.bloom = 0;
        look.flowerOpacity = 0;
    } else if (t < g.bloomPeak) {
        const double e = easeOut((t - bloomFrom) / (g.bloomPeak - bloomFrom));
        look.bloom = 1.18 * e;
        look.flowerOpacity = e;
    } else if (t < settled) {
        const double e = easeOut((t - g.bloomPeak) / (settled - g.bloomPeak));
        look.bloom = 1.18 + (1.0 - 1.18) * e;
        look.flowerOpacity = 1;
    } else {
        look.bloom = 1;
        look.flowerOpacity = 1;
    }
    return look;
}

void paintFlower(QPainter& p, const QPointF& centre, double radius) {
    // Its petals reach 6.6 + 3.7 flower units out.
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(centre);
    p.scale(radius / 10.3, radius / 10.3);
    daisy(p, geometry().line, geometry().flowerCentre);
    p.restore();
}

QPixmap picture(int size, qreal dpr) {
    QPixmap pixmap(qRound(size * dpr), qRound(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    Look look;
    look.route = size >= 24; // logo-walls under 24 px
    paint(p, QRectF(0, 0, size, size), look);
    return pixmap;
}

QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 20, 24, 32, 48, 64, 128, 256, 512}) {
        QPixmap pixmap(size, size);
        pixmap.fill(geometry().ground);
        QPainter p(&pixmap);
        Look look;
        look.route = size >= 24;
        // A little in from the tile's edges (the route starts below the
        // outer wall).
        const double inset = size * 0.1;
        paint(p, QRectF(inset, inset * 0.8, size - 2 * inset, size - 2 * inset), look);
        p.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace maze

MazeLogo::MazeLogo(int size, QWidget* pParent, bool route)
        : QWidget(pParent),
          m_route(route) {
    setFixedSize(size, size);
    setAttribute(Qt::WA_TranslucentBackground);
}

void MazeLogo::paintEvent(QPaintEvent*) {
    QPainter p(this);
    maze::Look look;
    look.route = m_route && width() >= 24; // logo-walls under 24 px
    maze::paint(p, QRectF(rect()), look);
}

MazeWait::MazeWait(QWidget* pParent)
        : QWidget(pParent) {
    setAttribute(Qt::WA_NoSystemBackground);
    m_frame.setInterval(16);
    connect(&m_frame, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    m_clock.start();
}

void MazeWait::setText(const QString& text) {
    m_text = text;
    update();
}

void MazeWait::setOpaque(bool opaque) {
    m_opaque = opaque;
    update();
}

void MazeWait::restart() {
    m_clock.restart();
    update();
}

void MazeWait::paintWait(QPainter& p, const QRectF& rect, qint64 ms, const QString& text) {
    // The maze in the middle (a little above it), the note under it.
    const double size = std::clamp(std::min(rect.height() * 0.36, rect.width() * 0.32), 64.0, 280.0);
    const double gap = text.isEmpty() ? 0 : size * 0.16 + 48;
    const QRectF box(rect.center().x() - size / 2, rect.center().y() - (size + gap) / 2 - size * 0.06, size, size);
    maze::paint(p, box, maze::loading(ms));
    if (!text.isEmpty()) {
        p.save();
        p.setFont(theme::font(theme::Face::Voice, 17));
        p.setPen(QColor(theme::kType));
        const double width = std::min(rect.width() - 32, 460.0);
        const QRectF words(rect.center().x() - width / 2, box.bottom() + size * 0.16, width, 120);
        p.drawText(words, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, text);
        p.restore();
    }
}

void MazeWait::paintEvent(QPaintEvent*) {
    QPainter p(this);
    if (m_opaque) {
        theme::paintPage(p, this, rect());
    }
    // In, over 200 ms (DESIGN.md 9), painted at its own opacity.
    p.setOpacity(std::min(1.0, m_clock.elapsed() / 200.0));
    paintWait(p, QRectF(rect()), m_clock.elapsed(), m_text);
}

void MazeWait::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    m_frame.start();
}

void MazeWait::hideEvent(QHideEvent* pEvent) {
    QWidget::hideEvent(pEvent);
    m_frame.stop();
}

} // namespace mixxx::b2b
