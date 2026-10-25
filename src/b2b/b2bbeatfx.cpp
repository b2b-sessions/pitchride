#include "b2b/b2bbeatfx.h"

#include <QDebug>
#include <QDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "effects/backends/builtin/b2bbeatfxdsp.h"
#include "effects/backends/builtin/b2bbeatfxeffect.h"

namespace mixxx::b2b {

namespace {
const QString kBooth = QStringLiteral("[B2BBooth]");
const QString kUnit = QStringLiteral("[EffectRack1_EffectUnit1]");
const QString kSlot = QStringLiteral("[EffectRack1_EffectUnit1_Effect1]");

// The effect's parameters (b2bbeatfxeffect.h, in order).
enum Param {
    kPFx = 1,
    kPBeats,
    kPTime,
    kPLevel,
    kPOn,
    kPLow,
    kPMid,
    kPHi,
    kPWave,
    kPQuantize,
    kPTap,
    kPXpad,
};

// The inputs, in CH SELECT order (as MixerControls routes them).
const char* const kInputs[] = {"[Channel1]",
        "[Channel2]",
        "[Channel3]",
        "[Channel4]",
        "[Microphone]",
        "[BusLeft]",
        "[BusRight]",
        "[Master]"};

constexpr qint64 kHoldMs = 600;
constexpr qint64 kButtonLitMs = 150;

double get(const QString& group, const QString& item) {
    return ControlObject::get(ConfigKey(group, item));
}

ConfigKey key(const char* item) {
    return ConfigKey(kBooth, QString::fromLatin1(item));
}

double frac(double x) {
    return x - std::floor(x);
}

bool isTriplet(int fx) {
    return fx == b2bfx::kTripletFilter || fx == b2bfx::kTripletRoll;
}

// ------------------------------------------------------------- the look --
// Display colours: white values, mid-grey labels, one accent.
const QColor kWhite(0xFF, 0xFF, 0xFF);
const QColor kLabel(0x9A, 0x9A, 0xA3);
const QColor kDim(0x6E, 0x6E, 0x78);
const QColor kAccent(0x3D, 0xB2, 0xFF);
const QColor kScreen(0x05, 0x06, 0x08);

enum class Face { Regular, Medium, SemiBold, Mono };

QFont face(Face f, double px, double spacing = 0) {
    QFont font;
    switch (f) {
    case Face::Regular:
        font.setFamily(QStringLiteral("Barlow Semi Condensed"));
        break;
    case Face::Medium:
        font.setFamily(QStringLiteral("Barlow Semi Condensed Medium"));
        break;
    case Face::SemiBold:
        font.setFamily(QStringLiteral("Barlow Semi Condensed SemiBold"));
        break;
    case Face::Mono:
        font.setFamily(QStringLiteral("JetBrains Mono Medium"));
        font.setStyleHint(QFont::Monospace);
        break;
    }
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(px))));
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferNoHinting); // even at fractional scales
    if (spacing != 0) {
        font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    }
    return font;
}

// Text in a rect, shrunk to fit its width.
void fitText(QPainter* p, QRectF rect, Qt::Alignment align, const QString& text, QFont font) {
    for (int i = 0; i < 12; ++i) {
        if (QFontMetricsF(font).horizontalAdvance(text) <= rect.width() || font.pixelSize() <= 6) {
            break;
        }
        font.setPixelSize(font.pixelSize() - 1);
    }
    p->setFont(font);
    p->drawText(rect, align, text);
}
} // namespace

// ------------------------------------------------------------- display --

/// The BEAT FX display: status tags and BPM, the effect, the beat row,
/// TIME, the X-PAD bar and the effect's own options.
class BeatFxScreen : public QWidget {
  public:
    BeatFxScreen(BeatFx* pFx, QWidget* pParent)
            : QWidget(pParent),
              m_pFx(pFx) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setToolTip(tr("BEAT FX display (MOBIUS: click SAW / TRIANGLE)"));
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.fillRect(rect(), kScreen);
        if (!m_pFx->isActive()) {
            return;
        }
        const BeatFx::View v = m_pFx->view();
        const double W = width();
        const double s = height() / 81.0; // the panel's screen is 81 skin pixels high
        const double pad = 4 * s;

        // Top row: QUANTIZE and AUTO, the BPM.
        double x = pad;
        auto tag = [&](const QString& text, bool lit) {
            const QFont font = face(Face::SemiBold, 4.6 * s, 0.8 * s / 1.2);
            const double w = QFontMetricsF(font).horizontalAdvance(text) + 3 * s;
            const QRectF r(x, 3.5 * s, w, 7 * s);
            p.setPen(Qt::NoPen);
            if (lit) {
                p.setBrush(kWhite);
                p.drawRoundedRect(r, 1.5 * s, 1.5 * s);
                p.setPen(Qt::black);
            } else {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kDim, std::max(1.0, 0.6 * s)));
                p.drawRoundedRect(r.adjusted(0.3 * s, 0.3 * s, -0.3 * s, -0.3 * s), 1.5 * s, 1.5 * s);
                p.setPen(kLabel);
            }
            p.setFont(font);
            p.drawText(r, Qt::AlignCenter, text);
            x += w + 1.8 * s;
        };
        tag(QStringLiteral("QUANTIZE"), v.quantize);
        tag(QStringLiteral("AUTO"), v.autoBpm);
        {
            const QFont unit = face(Face::Medium, 5.5 * s, 0.5 * s);
            const double unitW = QFontMetricsF(unit).horizontalAdvance(QStringLiteral("BPM"));
            const QRectF unitRect(W - pad - unitW, 2 * s, unitW, 10 * s);
            p.setFont(unit);
            p.setPen(kLabel);
            p.drawText(unitRect, Qt::AlignRight | Qt::AlignBottom, QStringLiteral("BPM"));
            const QString bpm = v.bpm > 0 ? QString::number(v.bpm, 'f', 1) : QStringLiteral("---.-");
            p.setPen(kWhite);
            fitText(&p,
                    QRectF(x, 1.5 * s, W - pad - unitW - 1.2 * s - x, 11 * s),
                    Qt::AlignRight | Qt::AlignBottom,
                    bpm,
                    face(Face::Mono, 8 * s));
        }

        // The effect.
        p.setPen(kWhite);
        fitText(&p,
                QRectF(pad, 13 * s, W - 2 * pad, 19 * s),
                Qt::AlignCenter,
                BeatFx::name(v.fx),
                face(Face::SemiBold, 15.5 * s, 0.5 * s));

        // The beat row: previous | CURRENT | next.
        {
            const QVector<double>& list = BeatFx::values(v.fx);
            const int at = m_pFx->valueIndex(v.fx, v.beats);
            const double rowY = 34 * s;
            const double rowH = 14 * s;
            const double midW = W * 0.40;
            const QRectF mid((W - midW) / 2, rowY, midW, rowH);
            const QRectF left(pad, rowY, mid.left() - pad - 2 * s, rowH);
            const QRectF right(mid.right() + 2 * s, rowY, W - pad - mid.right() - 2 * s, rowH);
            const bool manual = v.timeMs > 0 && !v.xpad;
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(manual ? kDim : kAccent, std::max(1.0, 0.8 * s)));
            p.drawRoundedRect(mid.adjusted(0.5 * s, 0.5 * s, -0.5 * s, -0.5 * s), 2 * s, 2 * s);
            p.setPen(manual ? kLabel : kWhite);
            fitText(&p, mid, Qt::AlignCenter, BeatFx::label(v.fx, list.value(at)), face(Face::Medium, 11 * s));
            p.setPen(kLabel);
            if (at > 0) {
                fitText(&p, left, Qt::AlignCenter, BeatFx::label(v.fx, list.value(at - 1)), face(Face::Medium, 8 * s));
            }
            if (at + 1 < list.size()) {
                fitText(&p, right, Qt::AlignCenter, BeatFx::label(v.fx, list.value(at + 1)), face(Face::Medium, 8 * s));
            }
        }

        // TIME: the value's real length (or the manual one).
        {
            const double rowY = 50.5 * s;
            const QRectF row(pad, rowY, W - 2 * pad, 10 * s);
            p.setPen(kLabel);
            p.setFont(face(Face::Medium, 5.5 * s, 0.5 * s));
            p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("TIME"));
            const bool reverb = v.fx == b2bfx::kReverb;
            const QString unit = reverb ? QStringLiteral("%") : QStringLiteral("msec");
            const QFont unitFont = face(Face::Regular, 5.5 * s);
            const double unitW = QFontMetricsF(unitFont).horizontalAdvance(unit);
            p.setFont(unitFont);
            p.drawText(row, Qt::AlignRight | Qt::AlignVCenter, unit);
            QString value;
            if (v.shownMs > 0) {
                value = QString::number(static_cast<int>(std::lround(v.shownMs)));
            } else {
                value = QStringLiteral("----");
            }
            p.setPen(kWhite);
            p.setFont(face(Face::Mono, 8.5 * s));
            p.drawText(row.adjusted(0, 0, -(unitW + 1.5 * s), 0), Qt::AlignRight | Qt::AlignVCenter, value);
        }

        // The X-PAD bar: lit while the pad is touched.
        {
            const QRectF bar(pad, 62 * s, W - 2 * pad, 7 * s);
            const QFont font = face(Face::SemiBold, 5 * s, 1.0 * s / 1.2);
            if (v.xpad) {
                p.setPen(Qt::NoPen);
                p.setBrush(kAccent);
                p.drawRoundedRect(bar, 1.5 * s, 1.5 * s);
                p.setPen(Qt::black);
            } else {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kDim, std::max(1.0, 0.6 * s)));
                p.drawRoundedRect(bar.adjusted(0.3 * s, 0.3 * s, -0.3 * s, -0.3 * s), 1.5 * s, 1.5 * s);
                p.setPen(kLabel);
            }
            p.setFont(font);
            p.drawText(bar, Qt::AlignCenter, QStringLiteral("X-PAD"));
        }

        // The effect's options.
        {
            const QRectF row(pad, 70.5 * s, W - 2 * pad, 9 * s);
            m_waveRects = {};
            if (v.fx == b2bfx::kMobius) {
                const QFont font = face(Face::Regular, 6.5 * s);
                const QStringList options = {QStringLiteral("SAW"), QStringLiteral("TRIANGLE")};
                const double half = row.width() / 2;
                for (int i = 0; i < 2; ++i) {
                    const QRectF r(row.left() + i * half, row.top(), half - 1.5 * s, row.height());
                    m_waveRects[i] = r;
                    const bool selected = v.wave == i;
                    if (selected) {
                        p.setPen(Qt::NoPen);
                        p.setBrush(QColor(0x2A, 0x2D, 0x34));
                        p.drawRoundedRect(r, 1.5 * s, 1.5 * s);
                    }
                    p.setPen(selected ? kWhite : kLabel);
                    p.setFont(font);
                    p.drawText(r, Qt::AlignCenter, options[i]);
                }
            } else {
                p.setPen(kLabel);
                p.setFont(face(Face::Regular, 6.5 * s));
                p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, parameterName(v.fx));
                p.setFont(face(Face::Mono, 6 * s));
                p.drawText(row,
                        Qt::AlignRight | Qt::AlignVCenter,
                        QString::number(static_cast<int>(std::lround(v.level * 100))) + QLatin1Char('%'));
            }
        }
    }

    void mousePressEvent(QMouseEvent* pEvent) override {
        for (int i = 0; i < 2; ++i) {
            if (m_waveRects[i].contains(pEvent->position())) {
                m_pFx->setWave(i);
                update();
            }
        }
    }

  private:
    static QString parameterName(int fx) {
        switch (fx) {
        case b2bfx::kDelay:
            return tr("Delay level");
        case b2bfx::kEcho:
        case b2bfx::kPingPong:
        case b2bfx::kSpiral:
            return tr("Level · feedback");
        case b2bfx::kHelix:
            return tr("Build");
        case b2bfx::kReverb:
            return tr("Size");
        case b2bfx::kTrans:
            return tr("Cut");
        case b2bfx::kRoll:
        case b2bfx::kTripletRoll:
            return tr("Roll level");
        default:
            return tr("Period");
        }
    }

    BeatFx* m_pFx;
    std::array<QRectF, 2> m_waveRects;
};

/// The X-PAD: touch (click and hold) to play the effect at the beat value
/// under the finger; slide to change it. Lights where it's touched.
class BeatFxPad : public QWidget {
  public:
    BeatFxPad(BeatFx* pFx, QWidget* pParent)
            : QWidget(pParent),
              m_pFx(pFx) {
        setToolTip(tr("X-PAD: hold for the effect at the beat value under the pointer; slide to change it"));
        setCursor(Qt::PointingHandCursor);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (!m_pFx->isActive()) {
            return;
        }
        const BeatFx::View v = m_pFx->view();
        if (!v.xpad) {
            return;
        }
        const QVector<double>& list = BeatFx::values(v.fx);
        const int n = std::max(1, static_cast<int>(list.size()));
        const int at = m_pFx->valueIndex(v.fx, v.beats);
        const double w = width() / static_cast<double>(n);
        const double cx = (at + 0.5) * w;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRadialGradient glow(QPointF(cx, height() / 2.0), std::max(w, height() * 0.9));
        QColor c = kAccent;
        c.setAlpha(210);
        glow.setColorAt(0, c);
        c.setAlpha(70);
        glow.setColorAt(0.45, c);
        c.setAlpha(0);
        glow.setColorAt(1, c);
        p.fillRect(rect(), glow);
    }
    void mousePressEvent(QMouseEvent* pEvent) override {
        m_pFx->padTouched(pEvent->position().x() / std::max(1, width()));
    }
    void mouseMoveEvent(QMouseEvent* pEvent) override {
        if (pEvent->buttons() & Qt::LeftButton) {
            m_pFx->padTouched(pEvent->position().x() / std::max(1, width()));
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        m_pFx->padReleased();
    }

  private:
    BeatFx* m_pFx;
};

// ---------------------------------------------------------------- BeatFx --

BeatFx::BeatFx(int decks, QObject* pParent)
        : QObject(pParent),
          m_numDecks(decks) {
    B2BBeatFxEffect::ensureTimingControls();
    m_clock.start();
    auto push = [](const char* item) {
        return std::make_unique<ControlPushButton>(key(item));
    };
    m_beatDown = push("beatfx_beat_down");
    m_beatUp = push("beatfx_beat_up");
    m_beatDownLight = std::make_unique<ControlObject>(key("beatfx_beat_down_light"));
    m_beatUpLight = std::make_unique<ControlObject>(key("beatfx_beat_up_light"));
    m_time = std::make_unique<ControlPotmeter>(key("beatfx_time"), 0.0, 1.0);
    m_on = push("beatfx_on"); // a press switches it on / off
    m_onLight = std::make_unique<ControlObject>(key("beatfx_on_light"));
    m_autoTap = push("beatfx_auto_tap");
    m_autoLight = std::make_unique<ControlObject>(key("beatfx_auto_light"));
    m_tap = push("beatfx_tap");
    m_tapLight = std::make_unique<ControlObject>(key("beatfx_tap_light"));
    m_quantize = push("beatfx_quantize");
    m_quantizeLight = std::make_unique<ControlObject>(key("beatfx_quantize_light"));
    const char* const bands[] = {"beatfx_low", "beatfx_mid", "beatfx_hi"};
    const char* const bandLights[] = {"beatfx_low_light", "beatfx_mid_light", "beatfx_hi_light"};
    for (int b = 0; b < 3; ++b) {
        m_band[b] = push(bands[b]);
        m_bandLight[b] = std::make_unique<ControlObject>(key(bandLights[b]));
        connect(m_band[b].get(), &ControlObject::valueChanged, this, [this, b](double v) {
            if (v > 0 && m_active) {
                setParam(kPLow + b, param(kPLow + b) > 0.5 ? 0.0 : 1.0);
            }
        });
    }
    for (int fx = 0; fx < b2bfx::kFxCount; ++fx) {
        const double start = fx == b2bfx::kReverb ? 50.0 : (isTriplet(fx) ? 2.0 / 3 : 1.0);
        m_valueIndex[fx] = valueIndex(fx, start);
    }

    connect(m_beatDown.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0) {
            stepBeat(-1);
            m_beatDownLit.start();
        }
    });
    connect(m_beatUp.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0) {
            stepBeat(1);
            m_beatUpLit.start();
        }
    });
    connect(m_time.get(), &ControlObject::valueChanged, this, &BeatFx::timeTurned);
    connect(m_on.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0) {
            m_onState = !m_onState;
            applyEngaged();
        }
    });
    connect(m_tap.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0) {
            tapPressed();
        }
    });
    connect(m_autoTap.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0) {
            m_autoTapHeld.start();
        } else {
            m_autoTapHeld.invalidate();
        }
    });
    connect(m_quantize.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0) {
            m_quantizeHeld.start();
            m_quantizeHoldDone = false;
        } else {
            if (m_quantizeHeld.isValid() && !m_quantizeHoldDone && m_active) {
                setParam(kPQuantize, param(kPQuantize) > 0.5 ? 0.0 : 1.0); // a press
            }
            m_quantizeHeld.invalidate();
        }
    });

    m_timer.setInterval(15);
    connect(&m_timer, &QTimer::timeout, this, &BeatFx::tick);
    m_timer.start();
}

BeatFx::~BeatFx() = default;

void BeatFx::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    m_pScreen.clear();
    m_pPad.clear();
}

void BeatFx::setActive(bool active) {
    m_active = active;
    if (active) {
        m_onState = false;
        setParam(kPOn, 0);
        setParam(kPXpad, 0);
        m_lastFx = static_cast<int>(std::lround(param(kPFx)));
    }
}

void BeatFx::reset() {
    m_onState = false;
    m_padHeld = false;
    m_padIndex = -1;
    // Each effect's beat back to where it starts, no TIME of its own.
    for (int fx = 0; fx < b2bfx::kFxCount; ++fx) {
        const double start = fx == b2bfx::kReverb ? 50.0 : (isTriplet(fx) ? 2.0 / 3 : 1.0);
        m_valueIndex[fx] = valueIndex(fx, start);
        m_manual[fx] = 0;
    }
    // The settings other than which effect (and its beat and time, which
    // pickedLocally sets): back to the effect's defaults. Never a passing
    // value for the effect itself: the friend's app hears every parameter,
    // and one of those arriving late would win.
    for (int n = kPLevel; n <= kPXpad; ++n) {
        if (ControlObject* pParam = ControlObject::getControl(
                    ConfigKey(kSlot, QStringLiteral("parameter%1").arg(n)), ControlFlag::NoWarnIfMissing)) {
            pParam->reset();
        }
    }
    setParam(kPOn, 0);
    m_time->setAndConfirm(m_time->defaultValue());
}

double BeatFx::param(int n) const {
    return get(kSlot, QStringLiteral("parameter%1").arg(n));
}

void BeatFx::setParam(int n, double value) {
    const ConfigKey k(kSlot, QStringLiteral("parameter%1").arg(n));
    if (ControlObject::get(k) != value) {
        ControlObject::set(k, value);
    }
}

// static
const QVector<double>& BeatFx::values(int fx) {
    static const QVector<double> beats = {
            1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 3.0 / 4, 1, 2, 4, 8, 16};
    static const QVector<double> triplets = {
            1.0 / 12, 1.0 / 6, 1.0 / 3, 2.0 / 3, 4.0 / 3, 8.0 / 3, 16.0 / 3};
    static const QVector<double> percent = {1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    if (fx == b2bfx::kReverb) {
        return percent;
    }
    return isTriplet(fx) ? triplets : beats;
}

// static
QString BeatFx::label(int fx, double value) {
    if (fx == b2bfx::kReverb) {
        return QString::number(static_cast<int>(std::lround(value))) + QLatin1Char('%');
    }
    if (value >= 1 && std::fabs(value - std::round(value)) < 1e-6) {
        return QString::number(static_cast<int>(std::lround(value)));
    }
    for (int d = 2; d <= 16; ++d) {
        const double n = value * d;
        if (std::fabs(n - std::round(n)) < 1e-6) {
            return QStringLiteral("%1/%2").arg(static_cast<int>(std::lround(n))).arg(d);
        }
    }
    return QString::number(value, 'f', 2);
}

// static
QString BeatFx::name(int fx) {
    static const char* const kNames[] = {"DELAY",
            "ECHO",
            "PING PONG",
            "SPIRAL",
            "HELIX",
            "REVERB",
            "FLANGER",
            "PHASER",
            "FILTER",
            "TRIPLET FILTER",
            "TRANS",
            "ROLL",
            "TRIPLET ROLL",
            "MOBIUS"};
    return QString::fromLatin1(kNames[std::clamp(fx, 0, b2bfx::kFxCount - 1)]);
}

int BeatFx::valueIndex(int fx, double value) const {
    const QVector<double>& list = values(fx);
    int best = 0;
    for (int i = 1; i < list.size(); ++i) {
        if (std::fabs(list[i] - value) < std::fabs(list[best] - value)) {
            best = i;
        }
    }
    return best;
}

BeatFx::View BeatFx::view() const {
    View v;
    v.fx = std::clamp(static_cast<int>(std::lround(param(kPFx))), 0, b2bfx::kFxCount - 1);
    v.beats = param(kPBeats);
    v.timeMs = param(kPTime);
    v.level = param(kPLevel);
    v.on = param(kPOn) > 0.5;
    v.xpad = param(kPXpad) > 0.5;
    v.quantize = param(kPQuantize) > 0.5;
    v.wave = param(kPWave) > 0.5 ? 1 : 0;
    const double tap = param(kPTap);
    v.autoBpm = tap <= 1;
    double bpm = 0;
    double phase = 0;
    sourceTempo(&bpm, &phase);
    v.bpm = bpm;
    v.phase = phase;
    if (v.fx == b2bfx::kReverb) {
        v.shownMs = v.timeMs > 0 ? v.timeMs : v.beats;
    } else if (v.timeMs > 0) {
        v.shownMs = v.timeMs;
    } else {
        v.shownMs = bpm > 0 ? v.beats * 60000.0 / bpm : 0;
    }
    return v;
}

void BeatFx::sourceTempo(double* pBpm, double* pPhase) const {
    const double tap = param(kPTap);
    if (tap > 1) {
        *pBpm = tap;
        const qint64 since = m_lastTap >= 0 ? m_clock.elapsed() - m_lastTap : 0;
        *pPhase = frac(m_tapPhaseBase + since * tap / 60000.0);
        return;
    }
    int channel = 8;
    for (int i = 0; i < 8; ++i) {
        if (get(kUnit, QStringLiteral("group_%1_enable").arg(QLatin1String(kInputs[i]))) > 0) {
            channel = i + 1;
            break;
        }
    }
    int deck = -1;
    if (channel <= 4) {
        deck = channel;
    } else {
        // The master, the mic or a crossfader side: the deck leading it
        // (playing, loudest; on that side of the crossfader).
        double best = -1;
        for (int d = 1; d <= m_numDecks; ++d) {
            const QString group = QStringLiteral("[Channel%1]").arg(d);
            if (get(group, QStringLiteral("track_loaded")) <= 0) {
                continue;
            }
            const double orientation = get(group, QStringLiteral("orientation"));
            if ((channel == 6 && orientation != 0) || (channel == 7 && orientation != 2)) {
                continue;
            }
            const double score = (get(group, QStringLiteral("play")) > 0 ? 2.0 : 0.0) +
                    get(group, QStringLiteral("volume"));
            if (score > best) {
                best = score;
                deck = d;
            }
        }
    }
    if (deck < 0) {
        *pBpm = 0;
        *pPhase = 0;
        return;
    }
    const QString group = QStringLiteral("[Channel%1]").arg(deck);
    *pBpm = get(group, QStringLiteral("bpm"));
    *pPhase = get(group, QStringLiteral("beat_distance"));
}

void BeatFx::pickedLocally(int fx) {
    if (!m_active) {
        return;
    }
    fx = std::clamp(fx, 0, b2bfx::kFxCount - 1);
    m_lastFx = fx;
    setParam(kPFx, fx);
    setParam(kPBeats, values(fx).value(m_valueIndex[fx]));
    setParam(kPTime, m_manual[fx]);
}

void BeatFx::stepBeat(int direction) {
    if (!m_active) {
        return;
    }
    const int fx = std::clamp(static_cast<int>(std::lround(param(kPFx))), 0, b2bfx::kFxCount - 1);
    if (param(kPTime) > 0) {
        // Back to beat timing from TIME.
        m_manual[fx] = 0;
        setParam(kPTime, 0);
        return;
    }
    const QVector<double>& list = values(fx);
    const int at = std::clamp(valueIndex(fx, param(kPBeats)) + direction, 0, static_cast<int>(list.size()) - 1);
    m_valueIndex[fx] = at;
    setParam(kPBeats, list[at]);
}

void BeatFx::timeTurned(double v) {
    if (!m_active) {
        return;
    }
    const int fx = std::clamp(static_cast<int>(std::lround(param(kPFx))), 0, b2bfx::kFxCount - 1);
    double value;
    if (fx == b2bfx::kReverb) {
        value = 1 + 99 * std::clamp(v, 0.0, 1.0);
    } else {
        value = std::clamp(std::pow(4000.0, std::clamp(v, 0.0, 1.0)), 1.0, 4000.0); // 1 .. 4000 ms
    }
    m_manual[fx] = value;
    setParam(kPTime, value);
}

void BeatFx::tapPressed() {
    if (!m_active) {
        return;
    }
    const qint64 now = m_clock.elapsed();
    if (!m_taps.isEmpty() && now - m_taps.last() > 2000) {
        m_taps.clear();
    }
    m_taps.append(now);
    while (m_taps.size() > 5) {
        m_taps.removeFirst();
    }
    m_lastTap = now;
    m_tapPhaseBase = 0; // the tap is the beat
    if (m_taps.size() >= 2) {
        const double avg = static_cast<double>(m_taps.last() - m_taps.first()) / (m_taps.size() - 1);
        setParam(kPTap, std::clamp(60000.0 / avg, 40.0, 300.0));
    }
}

void BeatFx::applyEngaged() {
    if (m_active) {
        setParam(kPOn, (m_onState || m_padHeld) ? 1.0 : 0.0);
    }
}

void BeatFx::padTouched(double x01) {
    if (!m_active) {
        return;
    }
    const int fx = std::clamp(static_cast<int>(std::lround(param(kPFx))), 0, b2bfx::kFxCount - 1);
    const QVector<double>& list = values(fx);
    const int n = static_cast<int>(list.size());
    const int at = std::clamp(static_cast<int>(std::floor(x01 * n)), 0, n - 1);
    if (at != m_padIndex) {
        m_padIndex = at;
        setParam(kPBeats, list[at]);
        setParam(kPTime, 0);
    }
    if (!m_padHeld) {
        m_padHeld = true;
        setParam(kPXpad, 1);
        applyEngaged();
    }
}

void BeatFx::padReleased() {
    if (!m_padHeld) {
        return;
    }
    m_padHeld = false;
    m_padIndex = -1;
    setParam(kPXpad, 0);
    applyEngaged(); // off, unless ON/OFF is on (tails ring on)
    const int fx = std::clamp(static_cast<int>(std::lround(param(kPFx))), 0, b2bfx::kFxCount - 1);
    setParam(kPBeats, values(fx).value(m_valueIndex[fx]));
    setParam(kPTime, m_manual[fx]);
}

void BeatFx::setWave(int wave) {
    if (m_active) {
        setParam(kPWave, wave);
    }
}

void BeatFx::openUtility() {
    auto* pDialog = new QDialog(m_pWindow);
    pDialog->setAttribute(Qt::WA_DeleteOnClose);
    pDialog->setWindowTitle(tr("UTILITY"));
    auto* pLayout = new QVBoxLayout(pDialog);
    auto* pTitle = new QLabel(tr("UTILITY"), pDialog);
    pTitle->setFont(face(Face::SemiBold, 20, 1));
    auto* pText = new QLabel(tr("Mixer settings will be here."), pDialog);
    pText->setFont(face(Face::Regular, 14));
    pLayout->addWidget(pTitle);
    pLayout->addWidget(pText);
    pDialog->resize(320, 160);
    pDialog->show();
}

void BeatFx::tick() {
    // The display and the pad, in the skin's slots.
    if (m_pWindow) {
        auto place = [this](auto& pWidget, const char* slotName, auto make) {
            QWidget* pSlot = m_pWindow->findChild<QWidget*>(QString::fromLatin1(slotName));
            if (pSlot && (!pWidget || pWidget->parentWidget() != pSlot)) {
                pWidget = make(pSlot);
                pWidget->setGeometry(pSlot->rect());
                pWidget->show();
                if (qEnvironmentVariableIsSet("B2B_SPEED_LOG")) { // testing: where it is
                    qInfo() << "B2B beatfx" << slotName << QRect(pSlot->mapToGlobal(QPoint(0, 0)), pSlot->size());
                }
            } else if (pSlot && pWidget && pWidget->geometry() != pSlot->rect()) {
                pWidget->setGeometry(pSlot->rect()); // the skin was scaled
            }
        };
        if (!m_pScreen || !m_pPad || (m_clock.elapsed() % 700) < 15) {
            place(m_pScreen, "BeatFxScreen", [this](QWidget* p) { return new BeatFxScreen(this, p); });
            place(m_pPad, "BeatFxPad", [this](QWidget* p) { return new BeatFxPad(this, p); });
        }
    }
    if (!m_active) {
        for (ControlObject* pLight : {m_onLight.get(), m_tapLight.get(), m_autoLight.get(), m_quantizeLight.get()}) {
            pLight->set(0);
        }
        return;
    }

    // The other DJ changed the effect: the FX SELECT knob shows it.
    const int fx = std::clamp(static_cast<int>(std::lround(param(kPFx))), 0, b2bfx::kFxCount - 1);
    if (fx != m_lastFx) {
        m_lastFx = fx;
        if (m_onRemoteFx) {
            m_onRemoteFx(fx);
        }
    }
    // ... or the input: CH SELECT shows it.
    int channel = 0;
    for (int i = 0; i < 8; ++i) {
        if (get(kUnit, QStringLiteral("group_%1_enable").arg(QLatin1String(kInputs[i]))) > 0) {
            channel = i + 1;
            break;
        }
    }
    const ConfigKey chKey(kBooth, QStringLiteral("beatfx_ch"));
    if (channel > 0 && ControlObject::get(chKey) != channel) {
        ControlObject::set(chKey, channel);
    }
    // ... or switched it on / off.
    const bool onParam = param(kPOn) > 0.5;
    if (!m_padHeld && param(kPXpad) < 0.5 && onParam != m_onState) {
        m_onState = onParam;
    }

    // The tempo and beat for the engine (master, mic, crossfader sides, taps).
    double bpm = 0;
    double phase = 0;
    sourceTempo(&bpm, &phase);
    ControlObject::set(key("beatfx_bpm"), bpm);
    ControlObject::set(key("beatfx_phase"), phase);

    // AUTO/TAP held: back to AUTO.
    if (m_autoTapHeld.isValid() && m_autoTapHeld.elapsed() >= kHoldMs) {
        setParam(kPTap, 0);
        m_taps.clear();
        m_autoTapHeld.invalidate();
    }
    // QUANTIZE held: UTILITY.
    if (m_quantizeHeld.isValid() && !m_quantizeHoldDone && m_quantizeHeld.elapsed() >= kHoldMs) {
        m_quantizeHoldDone = true;
        openUtility();
    }

    // The lights.
    const bool beatLit = bpm > 0 && phase < 0.5;
    m_onLight->set(onParam && (beatLit || bpm <= 0) ? 1.0 : 0.0);
    m_tapLight->set(bpm > 0 && phase < 0.2 ? 1.0 : 0.0);
    m_autoLight->set(param(kPTap) <= 1 ? 1.0 : 0.0);
    m_quantizeLight->set(param(kPQuantize) > 0.5 ? 1.0 : 0.0);
    for (int b = 0; b < 3; ++b) {
        m_bandLight[b]->set(param(kPLow + b) > 0.5 ? 1.0 : 0.0);
    }
    m_beatDownLight->set(m_beatDownLit.isValid() && m_beatDownLit.elapsed() < kButtonLitMs ? 1.0 : 0.0);
    m_beatUpLight->set(m_beatUpLit.isValid() && m_beatUpLit.elapsed() < kButtonLitMs ? 1.0 : 0.0);

    if (m_pScreen) {
        m_pScreen->update();
    }
    if (m_pPad) {
        m_pPad->update();
    }
}

} // namespace mixxx::b2b

#include "moc_b2bbeatfx.cpp"
