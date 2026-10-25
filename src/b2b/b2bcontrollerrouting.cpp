#include "b2b/b2bcontrollerrouting.h"

#include <QtDebug>
#include <atomic>

#include "moc_b2bcontrollerrouting.cpp"

namespace mixxx::b2b::routing {

namespace {
// Channel n's deck in bits 4n..4n+3. 0x3210: each channel its own deck.
constexpr uint32_t kStraight = 0x3210;
std::atomic<uint32_t> s_decks{kStraight};
std::atomic<bool> s_ignoreCrossfader{false};

const QLatin1String kChannel("[Channel");
} // namespace

bool set(const std::array<int, 4>& decks, bool ignoreCrossfader) {
    uint32_t packed = 0;
    for (int n = 0; n < 4; ++n) {
        packed |= static_cast<uint32_t>(decks[n] & 0x3) << (4 * n);
    }
    s_ignoreCrossfader.store(ignoreCrossfader, std::memory_order_release);
    const uint32_t before = s_decks.exchange(packed, std::memory_order_acq_rel);
    if (before == packed) {
        return false;
    }
    qInfo() << "B2B: controllers' channels 1-4 now play decks" << decks[0] + 1 << decks[1] + 1
            << decks[2] + 1 << decks[3] + 1;
    emit Notifier::instance()->changed();
    return true;
}

int deck(int channel) {
    if (channel < 0 || channel > 3) {
        return channel;
    }
    return static_cast<int>((s_decks.load(std::memory_order_acquire) >> (4 * channel)) & 0x3);
}

QString group(const QString& group) {
    const uint32_t packed = s_decks.load(std::memory_order_acquire);
    if (packed == kStraight || !group.contains(kChannel)) {
        return group;
    }
    // Every "[ChannelN]" (N 1..4) in it, all at once (1 and 3 can swap).
    QString out = group;
    int at = out.indexOf(kChannel);
    while (at >= 0) {
        const int digit = at + kChannel.size();
        if (digit + 1 < out.size() && out[digit + 1] == QLatin1Char(']') &&
                out[digit] >= QLatin1Char('1') && out[digit] <= QLatin1Char('4')) {
            const int channel = out[digit].digitValue() - 1;
            const int to = static_cast<int>((packed >> (4 * channel)) & 0x3);
            out[digit] = QChar(QLatin1Char('1').unicode() + to);
        }
        at = out.indexOf(kChannel, digit);
    }
    return out;
}

ConfigKey key(const ConfigKey& key) {
    if (s_decks.load(std::memory_order_acquire) == kStraight) {
        return key;
    }
    return ConfigKey(group(key.group), group(key.item));
}

bool ignored(const ConfigKey& routedKey) {
    return s_ignoreCrossfader.load(std::memory_order_acquire) &&
            routedKey.item == QLatin1String("crossfader") &&
            (routedKey.group == QLatin1String("[Master]") || routedKey.group == QLatin1String("[Mixer]"));
}

Notifier* Notifier::instance() {
    static Notifier* s_pNotifier = new Notifier();
    return s_pNotifier;
}

} // namespace mixxx::b2b::routing
