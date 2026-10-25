#include "b2b/b2bletters.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintDevice>
#include <QPainter>
#include <QRectF>
#include <QtGlobal>

namespace b2b {

namespace {

// A line this many device pixels in font size or more is snapped to whole
// pixels; smaller ones are drawn smooth at their exact size, from a face
// this big scaled down.
constexpr double kSnapPixels = 4.75;
constexpr int kSmallFacePixels = 48;

QString lettersPath(const QString& picturePath) {
    const QFileInfo info(picturePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".letters.json"));
}

} // namespace

std::shared_ptr<const Letters> Letters::forPicture(const QString& picturePath) {
    // One load per file (a picture can be shown by several groups, and a
    // skin is parsed again on every 2 / 4 deck switch).
    static QHash<QString, std::shared_ptr<const Letters>> s_loaded;
    const QString path = lettersPath(picturePath);
    const auto known = s_loaded.constFind(path);
    if (known != s_loaded.constEnd()) {
        return known.value();
    }
    std::shared_ptr<const Letters> pLetters;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        auto pNew = std::make_shared<Letters>();
        pNew->m_width = root.value(QStringLiteral("w")).toDouble();
        pNew->m_height = root.value(QStringLiteral("h")).toDouble();
        pNew->m_face = root.value(QStringLiteral("face")).toString(QStringLiteral("DM Mono"));
        pNew->m_weight = root.value(QStringLiteral("weight")).toInt(500);
        const QJsonArray lines = root.value(QStringLiteral("lines")).toArray();
        for (const QJsonValue& value : lines) {
            const QJsonObject o = value.toObject();
            Line line;
            line.text = o.value(QStringLiteral("text")).toString();
            line.x = o.value(QStringLiteral("x")).toDouble();
            line.y = o.value(QStringLiteral("y")).toDouble();
            line.size = o.value(QStringLiteral("size")).toDouble();
            const QString anchor = o.value(QStringLiteral("anchor")).toString();
            line.anchor = anchor == QStringLiteral("start") ? -1 : anchor == QStringLiteral("end") ? 1 : 0;
            line.squeeze = o.value(QStringLiteral("squeeze")).toDouble(1.0);
            line.colour = QColor(o.value(QStringLiteral("colour")).toString(QStringLiteral("#e9e5dd")));
            if (!line.text.isEmpty() && line.size > 0) {
                pNew->m_lines.append(line);
            }
        }
        const QJsonArray rules = root.value(QStringLiteral("rules")).toArray();
        for (const QJsonValue& value : rules) {
            const QJsonObject o = value.toObject();
            Rule rule;
            rule.x = o.value(QStringLiteral("x")).toDouble();
            rule.y = o.value(QStringLiteral("y")).toDouble();
            rule.w = o.value(QStringLiteral("w")).toDouble();
            rule.h = o.value(QStringLiteral("h")).toDouble();
            rule.colour = QColor(o.value(QStringLiteral("colour")).toString(QStringLiteral("#e9e5dd")));
            if (rule.w > 0 && rule.h > 0) {
                pNew->m_rules.append(rule);
            }
        }
        if (pNew->m_width > 0 && pNew->m_height > 0 &&
                (!pNew->m_lines.isEmpty() || !pNew->m_rules.isEmpty())) {
            pLetters = pNew;
        } else {
            qWarning() << "Letters: nothing to draw in" << path;
        }
    }
    s_loaded.insert(path, pLetters);
    return pLetters;
}

void Letters::draw(QPainter* pPainter, const QRectF& target) const {
    if (!pPainter || target.isEmpty() || (m_lines.isEmpty() && m_rules.isEmpty())) {
        return;
    }
    // Work in device pixels: the painter's own scale (a zoom draws through
    // one) and the screen's pixel ratio, so a whole pixel here is a whole
    // pixel on the screen.
    const QPaintDevice* pDevice = pPainter->device();
    const double ratio = pDevice ? pDevice->devicePixelRatioF() : 1.0;
    const QTransform transform = pPainter->transform();
    const QPointF origin = transform.map(target.topLeft()) * ratio;
    const double scaleX = target.width() / m_width * transform.m11() * ratio;
    const double scaleY = target.height() / m_height * transform.m22() * ratio;
    if (scaleX <= 0 || scaleY <= 0) {
        return;
    }

    pPainter->save();
    pPainter->resetTransform();
    pPainter->scale(1.0 / ratio, 1.0 / ratio);
    pPainter->setRenderHint(QPainter::TextAntialiasing, true);
    // The rules: each edge on a pixel, at least a pixel tall (thinner, a
    // line comes out a different grey depending where it falls).
    pPainter->setRenderHint(QPainter::Antialiasing, false);
    for (const Rule& rule : m_rules) {
        const int left = qRound(origin.x() + rule.x * scaleX);
        const int right = qRound(origin.x() + (rule.x + rule.w) * scaleX);
        const int height = qMax(1, qRound(rule.h * scaleY));
        const int top = qRound(origin.y() + (rule.y + rule.h / 2.0) * scaleY - height / 2.0);
        pPainter->fillRect(QRect(left, top, qMax(1, right - left), height), rule.colour);
    }
    for (const Line& line : m_lines) {
        const double pixels = line.size * scaleY;
        // Big enough for its strokes to land on pixels: a whole-pixel size,
        // hinted, on a whole pixel. Smaller, hinting only breaks the glyphs
        // up: its exact size, smooth, where it falls (drawn from a larger
        // face, scaled down).
        const bool snap = pixels >= kSnapPixels;
        const int fontPixels = snap ? qRound(pixels) : kSmallFacePixels;
        const double down = snap ? 1.0 : pixels / kSmallFacePixels;
        QFont font(m_face);
        font.setWeight(static_cast<QFont::Weight>(m_weight));
        font.setPixelSize(fontPixels);
        font.setHintingPreference(snap ? QFont::PreferFullHinting : QFont::PreferNoHinting);
        if (line.squeeze < 0.995) {
            font.setStretch(qBound(50, qRound(line.squeeze * 100.0), 100));
        }
        const QFontMetricsF metrics(font);
        const double width = metrics.horizontalAdvance(line.text) * down;
        const double anchorX = origin.x() + line.x * scaleX;
        const double left = line.anchor < 0 ? anchorX : line.anchor > 0 ? anchorX - width : anchorX - width / 2.0;
        const double baseline = origin.y() + line.y * scaleY;
        pPainter->setFont(font);
        pPainter->setPen(line.colour);
        if (snap) {
            pPainter->drawText(QPointF(qRound(left), qRound(baseline)), line.text);
        } else {
            pPainter->save();
            pPainter->translate(left, baseline);
            pPainter->scale(down, down);
            pPainter->drawText(QPointF(0, 0), line.text);
            pPainter->restore();
        }
    }
    pPainter->restore();
}

} // namespace b2b
