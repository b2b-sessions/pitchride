#include "skin/legacy/launchimage.h"

#include <QFontDatabase>
#include <QPainter>
#include <algorithm>

#include "b2b/b2bmaze.h"
#include "b2b/b2btheme.h"
#include "moc_launchimage.cpp"
#include "preferences/configobject.h"

namespace {

// The faces the screen uses, registered now: it shows before the app's
// fonts are loaded (registering them twice is harmless).
void registerFonts() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    const QString fonts = ConfigObject<ConfigValue>::computeResourcePath() + QStringLiteral("fonts/");
    for (const char* file : {"HankenGrotesk-ExtraBold.ttf", "DMMono-Regular.ttf"}) {
        QFontDatabase::addApplicationFont(fonts + QLatin1String(file));
    }
}

constexpr double kMaze = 200;      // the maze (px)
constexpr double kBarWidth = 240;  // the loading bar
constexpr double kBarHeight = 2;

} // namespace

LaunchImage::LaunchImage(QWidget* pParent, const QString& styleSheet)
        : QWidget(pParent) {
    Q_UNUSED(styleSheet);
    registerFonts();
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_clock.start();
    // The maze moves while there is time to paint (the start-up work runs
    // on this thread: between its steps, and while the skin loads after).
    m_frame.setInterval(16);
    connect(&m_frame, &QTimer::timeout, this, [this] {
        m_shown += (m_target - m_shown) * 0.2;
        update();
    });
    m_frame.start();
}

void LaunchImage::progress(int value, const QString& serviceName) {
    m_target = std::clamp(value, 0, 100);
    m_service = serviceName;
    // Painted now: the start-up work only lets the screen paint between its
    // steps, so the bar moves on each step rather than catching up later.
    m_shown = std::max(m_shown, m_target - 8.0);
    update();
}

void LaunchImage::paintEvent(QPaintEvent*) {
    using namespace mixxx::b2b;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    theme::paintPage(p, this, rect());

    // The maze, the wordmark under it, the bar under that: centred as one.
    const QFont word = theme::font(theme::Face::Display, 40);
    const QFontMetricsF wordMetrics(word);
    const double wordHeight = wordMetrics.capHeight();
    const double total = kMaze + 28 + wordHeight + 30 + kBarHeight + 14 + 12;
    const double top = (height() - total) / 2.0 - height() * 0.04;
    const double cx = width() / 2.0;

    const QRectF mazeBox(cx - kMaze / 2, top, kMaze, kMaze);
    maze::paint(p, mazeBox, maze::loading(m_clock.elapsed()));

    // pitchride, lowercase, Hanken Grotesk 800, tight (DESIGN.md 3, 4).
    QFont tight = word;
    tight.setLetterSpacing(QFont::AbsoluteSpacing, -0.04 * 40); // as the top bar's
    p.setFont(tight);
    p.setPen(QColor(theme::kType));
    const QString name = QStringLiteral("pitchride");
    const double nameWidth = QFontMetricsF(tight).horizontalAdvance(name);
    const double baseline = mazeBox.bottom() + 28 + wordHeight;
    p.drawText(QPointF(cx - nameWidth / 2, baseline), name);

    // The bar: a faint line, the part done in the action colour. Square.
    const QRectF bar(qRound(cx - kBarWidth / 2), qRound(baseline + 30), kBarWidth, kBarHeight);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(bar, QColor(theme::kLine));
    const double done = std::clamp(m_shown, 0.0, 100.0) / 100.0;
    p.fillRect(QRectF(bar.left(), bar.top(), qRound(kBarWidth * done), kBarHeight), QColor(theme::kAction));

    // What is loading, in the label face (uppercase micro label).
    if (!m_service.isEmpty()) {
        p.setFont(theme::font(theme::Face::Label, 11));
        p.setPen(QColor(theme::kTypeMuted));
        const QString what = QStringLiteral("LOADING · %1").arg(m_service.toUpper());
        p.drawText(QRectF(cx - 200, bar.bottom() + 12, 400, 20), Qt::AlignHCenter | Qt::AlignTop, what);
    }
}
