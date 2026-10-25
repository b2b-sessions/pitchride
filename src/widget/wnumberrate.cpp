#include "widget/wnumberrate.h"

#include "b2b/b2btemporange.h"
#include "control/controlproxy.h"
#include "moc_wnumberrate.cpp"
#include "util/math.h"

namespace {

inline QChar sign(double number) {
    if (number > 0) {
        return '+';
    }
    if (number < 0) {
        return '-';
    }
    return ' ';
}

} // namespace

WNumberRate::WNumberRate(const QString& group, QWidget* parent)
        : WNumber(parent) {
    m_pRateRatio = new ControlProxy(group, "rate_ratio", this, ControlFlag::NoAssertIfMissing);
    m_pRateRatio->connectValueChanged(this, &WNumberRate::setValue);
    m_pRateRange = new ControlProxy(group, "rateRange", this, ControlFlag::NoAssertIfMissing);
    m_pRateRange->connectValueChanged(this, [this](double) {
        setValue(m_pRateRatio->get());
    });
}

void WNumberRate::setup(const QDomNode& node, const SkinContext& context) {
    WNumber::setup(node, context);

    // Initialize the widget (overrides the base class initial value.
    setValue(m_pRateRatio->get());
}

void WNumberRate::setValue(double dValue) {
    const double digitFactor = pow(10, m_iNoDigits);
    // Calculate percentage rounded to the number of digits specified by iNoDigits
    const double percentage = round((dValue - 1) * 100.0 * digitFactor) / digitFactor;
    if (m_skinText.contains(QStringLiteral("%1"))) {
        // B2B: "+8.00", "-1.24", "0.00" (no sign at zero) and the range.
        const QString number = (percentage != 0 ? QString(sign(percentage)) : QString()) +
                QString::number(fabs(percentage), 'f', m_iNoDigits);
        QString text = m_skinText;
        text.replace(QStringLiteral("%1"), number);
        text.replace(QStringLiteral("%2"), mixxx::b2b::tempo::label(m_pRateRange->get()));
        setText(text);
        return;
    }
    setText(m_skinText + sign(percentage) + QString::number(fabs(percentage), 'f', m_iNoDigits));
}
