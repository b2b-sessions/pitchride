#include "b2b/b2bmappingwizard.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <algorithm>
#include <atomic>

#include "controllers/controller.h"
#include "controllers/controllermanager.h"
#include "controllers/defs_controllers.h"
#include "controllers/legacycontrollermappingfilehandler.h"
#include "controllers/midi/legacymidicontrollermapping.h"
#include "controllers/midi/midicontroller.h"
#include "moc_b2bmappingwizard.cpp"

namespace mixxx::b2b::mapwizard {

namespace {
std::atomic<bool> s_inRoom{false};

const QString kDot = QString::fromUtf8(" \xC2\xB7 ");
const QLatin1String kYoursSuffix(" (yours)");

/// As ControllerManager keeps a device in the settings.
QString deviceKey(::Controller* pController) {
    QString name = pController->getName();
    return name.replace(' ', '_').replace('/', '_').replace('\\', '_');
}

ConfigKey originalKey(::Controller* pController) {
    return ConfigKey(QStringLiteral("[Controller]"), QStringLiteral("b2b_original_") + deviceKey(pController));
}

/// "loop_in" -> "LOOP IN".
QString words(QString item) {
    return item.replace('_', ' ').toUpper();
}

/// A mapping file the settings name (a path, or a file name in the user's
/// or the bundled mappings) as a full path ("" if it isn't there).
QString resolve(UserSettingsPointer pConfig, const QString& pathOrName) {
    if (pathOrName.isEmpty()) {
        return QString();
    }
    const QFileInfo info(pathOrName);
    if (info.isAbsolute()) {
        return info.exists() ? info.absoluteFilePath() : QString();
    }
    for (const QString& dir : {userMappingsPath(pConfig), resourceMappingsPath(pConfig)}) {
        const QFileInfo inDir(QDir(dir).filePath(pathOrName));
        if (inDir.exists()) {
            return inDir.absoluteFilePath();
        }
    }
    return QString();
}

std::shared_ptr<LegacyMidiControllerMapping> load(UserSettingsPointer pConfig, const QString& path) {
    if (path.isEmpty()) {
        return nullptr;
    }
    return std::dynamic_pointer_cast<LegacyMidiControllerMapping>(
            LegacyControllerMappingFileHandler::loadMapping(
                    QFileInfo(path), QDir(resourceMappingsPath(pConfig))));
}

/// The same binding (what it controls and how), whatever its description.
bool sameBinding(const MidiInputMapping& a, const MidiInputMapping& b) {
    return a.key == b.key && a.options == b.options && a.control == b.control;
}

/// The controller uses this mapping from now (on its own thread, as the
/// Controllers page does it), and the settings remember it: written now,
/// not only when the app quits, so a crash doesn't lose it.
void apply(UserSettingsPointer pConfig,
        ControllerManager* pManager,
        ::Controller* pController,
        std::shared_ptr<LegacyMidiControllerMapping> pMapping) {
    QMetaObject::invokeMethod(
            pManager,
            [pConfig, pManager, pController, pMapping]() {
                pManager->slotApplyMapping(pController, pMapping, true);
                pConfig->save();
            },
            Qt::QueuedConnection);
    emit Notifier::instance()->mappingsChanged();
}

bool write(std::shared_ptr<LegacyMidiControllerMapping> pMapping, const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!pMapping->saveMapping(path)) {
        qWarning() << "B2B: couldn't save the mapping as" << path;
        return false;
    }
    pMapping->setFilePath(path);
    pMapping->setDirty(false);
    return true;
}
} // namespace

QString controlName(const ConfigKey& key) {
    const QString& g = key.group;
    const QString& item = key.item;
    static const QRegularExpression kDeck(QStringLiteral("^\\[Channel([1-4])\\]$"));
    static const QRegularExpression kEq(QStringLiteral("^\\[EqualizerRack1_\\[Channel([1-4])\\]_Effect1\\]$"));
    static const QRegularExpression kQuick(QStringLiteral("^\\[QuickEffectRack1_\\[Channel([1-4])\\]\\]$"));
    static const QRegularExpression kHotcue(QStringLiteral("^hotcue_([1-8])_(activate|clear)$"));
    static const QRegularExpression kBeatloop(QStringLiteral("^beatloop_([0-9.]+)_(activate|toggle)$"));
    static const QRegularExpression kBoothDeck(QStringLiteral("^deck([1-4])_(.+)$"));
    static const QRegularExpression kColor(QStringLiteral("^color([1-4])$"));
    const QString mixer = QStringLiteral("MIXER") + kDot;

    QRegularExpressionMatch m = kDeck.match(g);
    if (m.hasMatch()) {
        const QString ch = m.captured(1);
        // The channel's row on the mixer.
        if (item == QLatin1String("volume")) {
            return mixer + QStringLiteral("CH %1 FADER").arg(ch);
        }
        if (item == QLatin1String("pregain")) {
            return mixer + QStringLiteral("CH %1 TRIM").arg(ch);
        }
        if (item == QLatin1String("pfl")) {
            return mixer + QStringLiteral("CH %1 CUE").arg(ch);
        }
        if (item == QLatin1String("orientation")) {
            return mixer + QStringLiteral("CH %1 CROSSFADER ASSIGN").arg(ch);
        }
        // The player.
        const QString deck = QStringLiteral("DECK %1").arg(ch) + kDot;
        static const QHash<QString, QString> kPlayer = {
                {QStringLiteral("play"), QStringLiteral("PLAY/PAUSE")},
                {QStringLiteral("cue_default"), QStringLiteral("CUE")},
                {QStringLiteral("sync_enabled"), QStringLiteral("BEAT SYNC")},
                {QStringLiteral("rate"), QStringLiteral("TEMPO")},
                {QStringLiteral("keylock"), QStringLiteral("MASTER TEMPO")},
                {QStringLiteral("loop_in"), QStringLiteral("LOOP IN")},
                {QStringLiteral("loop_out"), QStringLiteral("LOOP OUT")},
                {QStringLiteral("reloop_toggle"), QStringLiteral("RELOOP/EXIT")},
                {QStringLiteral("loop_halve"), QStringLiteral("LOOP 1/2X")},
                {QStringLiteral("loop_double"), QStringLiteral("LOOP 2X")},
                {QStringLiteral("beatjump_backward"), QStringLiteral("BEAT JUMP BACK")},
                {QStringLiteral("beatjump_forward"), QStringLiteral("BEAT JUMP FORWARD")},
                {QStringLiteral("slip_enabled"), QStringLiteral("SLIP")},
                {QStringLiteral("reverse"), QStringLiteral("REVERSE")},
                {QStringLiteral("eject"), QStringLiteral("EJECT")},
                {QStringLiteral("LoadSelectedTrack"), QStringLiteral("LOAD")},
                {QStringLiteral("pitch_adjust"), QStringLiteral("KEY")},
        };
        if (kPlayer.contains(item)) {
            return deck + kPlayer.value(item);
        }
        const QRegularExpressionMatch cue = kHotcue.match(item);
        if (cue.hasMatch()) {
            // The CDJ's pads: A..H.
            const QChar letter(QLatin1Char('A').unicode() + cue.captured(1).toInt() - 1);
            return deck + (cue.captured(2) == QLatin1String("clear") ? QStringLiteral("DELETE HOT CUE %1")
                                                                     : QStringLiteral("HOT CUE %1"))
                                  .arg(letter);
        }
        const QRegularExpressionMatch loop = kBeatloop.match(item);
        if (loop.hasMatch()) {
            return deck + QStringLiteral("%1 BEAT LOOP").arg(loop.captured(1));
        }
        return deck + words(item);
    }
    m = kEq.match(g);
    if (m.hasMatch()) {
        static const char* const kBands[] = {"LOW", "MID", "HI"};
        const QRegularExpressionMatch p = QRegularExpression(QStringLiteral("^(button_)?parameter([1-3])$")).match(item);
        if (p.hasMatch()) {
            return mixer + QStringLiteral("CH %1 %2%3")
                                   .arg(m.captured(1),
                                           QLatin1String(kBands[p.captured(2).toInt() - 1]),
                                           p.captured(1).isEmpty() ? QString() : QStringLiteral(" KILL"));
        }
        return mixer + QStringLiteral("CH %1 EQ ").arg(m.captured(1)) + words(item);
    }
    m = kQuick.match(g);
    if (m.hasMatch()) {
        return mixer + QStringLiteral("CH %1 COLOR").arg(m.captured(1));
    }
    if (g == QLatin1String("[B2BBooth]")) {
        if (item.startsWith(QLatin1String("beatfx_"))) {
            static const QHash<QString, QString> kBeatFx = {
                    {QStringLiteral("beatfx_on"), QStringLiteral("ON/OFF")},
                    {QStringLiteral("beatfx_time"), QStringLiteral("TIME")},
                    {QStringLiteral("beatfx_select"), QStringLiteral("FX SELECT")},
                    {QStringLiteral("beatfx_ch"), QStringLiteral("CH SELECT")},
                    {QStringLiteral("beatfx_beat_down"), QStringLiteral("BEAT LEFT")},
                    {QStringLiteral("beatfx_beat_up"), QStringLiteral("BEAT RIGHT")},
                    {QStringLiteral("beatfx_tap"), QStringLiteral("TAP")},
                    {QStringLiteral("beatfx_auto_tap"), QStringLiteral("AUTO/TAP")},
                    {QStringLiteral("beatfx_quantize"), QStringLiteral("QUANTIZE")},
            };
            return QStringLiteral("BEAT FX") + kDot + kBeatFx.value(item, words(item.mid(7)));
        }
        const QRegularExpressionMatch color = kColor.match(item);
        if (color.hasMatch()) {
            return mixer + QStringLiteral("CH %1 COLOR").arg(color.captured(1));
        }
        const QRegularExpressionMatch deck = kBoothDeck.match(item);
        if (deck.hasMatch()) {
            return QStringLiteral("DECK %1").arg(deck.captured(1)) + kDot + words(deck.captured(2));
        }
        return mixer + words(item);
    }
    if (g == QLatin1String("[EffectRack1_EffectUnit1_Effect1]") && item == QLatin1String("parameter4")) {
        return QStringLiteral("BEAT FX") + kDot + QStringLiteral("LEVEL/DEPTH");
    }
    if (g == QLatin1String("[Master]") || g == QLatin1String("[Mixer]")) {
        static const QHash<QString, QString> kMaster = {
                {QStringLiteral("crossfader"), QStringLiteral("CROSSFADER")},
                {QStringLiteral("gain"), QStringLiteral("MASTER LEVEL")},
                {QStringLiteral("headMix"), QStringLiteral("HEADPHONES MIX")},
                {QStringLiteral("headGain"), QStringLiteral("HEADPHONES LEVEL")},
                {QStringLiteral("booth_gain"), QStringLiteral("BOOTH LEVEL")},
        };
        return mixer + kMaster.value(item, words(item));
    }
    if (g == QLatin1String("[Library]")) {
        return QStringLiteral("BROWSE") + kDot + words(item);
    }
    return QString(); // not on the decks or the mixer
}

QList<::Controller*> midiControllers(ControllerManager* pManager) {
    QList<::Controller*> out;
    if (!pManager) {
        return out;
    }
    const QList<::Controller*> all = pManager->getControllers();
    for (::Controller* pController : all) {
        if (qobject_cast<MidiController*>(pController) && pController->isInputDevice()) {
            out << pController;
        }
    }
    return out;
}

Files files(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController) {
    Files f;
    if (!pManager || !pController) {
        return f;
    }
    f.current = resolve(pConfig, pManager->getConfiguredMappingFileForDevice(pController->getName()));
    if (const auto pMapping = load(pConfig, f.current)) {
        f.currentName = pMapping->name();
    }
    f.yours = !f.current.isEmpty() && f.currentName.endsWith(kYoursSuffix) &&
            QFileInfo(f.current).absolutePath() == QDir(userMappingsPath(pConfig)).absolutePath();
    f.original = f.yours ? resolve(pConfig, pConfig->getValueString(originalKey(pController))) : QString();
    return f;
}

QList<Change> changes(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController) {
    QList<Change> out;
    const Files f = files(pConfig, pManager, pController);
    if (!f.yours) {
        return out;
    }
    const auto pYours = load(pConfig, f.current);
    const auto pOriginal = load(pConfig, f.original);
    if (!pYours) {
        return out;
    }
    const auto& yours = pYours->getInputMappings();
    for (auto it = yours.constBegin(); it != yours.constEnd(); ++it) {
        const ConfigKey* pKey = std::get_if<ConfigKey>(&it.value().control);
        if (!pKey) {
            continue; // a script's: the original's
        }
        bool inOriginal = false;
        if (pOriginal) {
            const auto range = pOriginal->getInputMappings().equal_range(it.key());
            for (auto o = range.first; o != range.second; ++o) {
                inOriginal = inOriginal || sameBinding(o.value(), it.value());
            }
        }
        if (inOriginal) {
            continue;
        }
        auto same = std::find_if(out.begin(), out.end(), [pKey](const Change& c) {
            return c.control == *pKey;
        });
        if (same == out.end()) {
            out << Change{{it.key()}, *pKey, controlName(*pKey)};
        } else if (!same->midiKeys.contains(it.key())) {
            same->midiKeys << it.key();
        }
    }
    std::sort(out.begin(), out.end(), [](const Change& a, const Change& b) {
        return a.name < b.name;
    });
    return out;
}

bool save(UserSettingsPointer pConfig,
        ControllerManager* pManager,
        ::Controller* pController,
        const MidiInputMappings& learned,
        QString* pName) {
    if (!pManager || !pController || learned.isEmpty()) {
        return false;
    }
    const Files f = files(pConfig, pManager, pController);
    std::shared_ptr<LegacyMidiControllerMapping> pMapping = load(pConfig, f.current);
    if (!pMapping) {
        pMapping = std::make_shared<LegacyMidiControllerMapping>();
    }
    // Learned last wins: whatever that input did before is replaced.
    for (const MidiInputMapping& mapping : learned) {
        pMapping->removeInputMapping(mapping.key.key);
    }
    for (const MidiInputMapping& mapping : learned) {
        pMapping->addInputMapping(mapping.key.key, mapping);
    }
    QString path = f.current;
    if (!f.yours) {
        // A new copy, named for what it was made from.
        const QString base = f.currentName.isEmpty() ? pController->getName() : f.currentName;
        pMapping->setName(base + kYoursSuffix);
        pMapping->setDescription(QStringLiteral("Made with PitchRide's mapping wizard from %1.")
                                         .arg(f.currentName.isEmpty() ? QStringLiteral("no mapping")
                                                                      : f.currentName));
        QString fileName = pMapping->name();
        fileName.replace('/', '-').replace('\\', '-');
        path = QDir(userMappingsPath(pConfig)).filePath(fileName + QStringLiteral(".midi.xml"));
        pConfig->set(originalKey(pController), f.current);
    }
    if (!write(pMapping, path)) {
        return false;
    }
    qInfo() << "B2B: the mapping wizard saved" << pMapping->name() << "with" << learned.size()
            << "learned input(s)";
    if (pName) {
        *pName = pMapping->name();
    }
    apply(pConfig, pManager, pController, pMapping);
    return true;
}

bool removeChange(UserSettingsPointer pConfig,
        ControllerManager* pManager,
        ::Controller* pController,
        const QList<uint16_t>& midiKeys) {
    const Files f = files(pConfig, pManager, pController);
    if (!f.yours) {
        return false;
    }
    const auto pYours = load(pConfig, f.current);
    if (!pYours) {
        return false;
    }
    const auto pOriginal = load(pConfig, f.original);
    for (const uint16_t midiKey : midiKeys) {
        pYours->removeInputMapping(midiKey);
        if (pOriginal) {
            const auto range = pOriginal->getInputMappings().equal_range(midiKey);
            for (auto it = range.first; it != range.second; ++it) {
                pYours->addInputMapping(midiKey, it.value());
            }
        }
    }
    if (!write(pYours, f.current)) {
        return false;
    }
    apply(pConfig, pManager, pController, pYours);
    return true;
}

void switchOn(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController) {
    if (!pManager || !pController) {
        return;
    }
    const QString path = files(pConfig, pManager, pController).current;
    QMetaObject::invokeMethod(
            pManager,
            [pConfig, pManager, pController, path]() {
                if (pController->isOpen()) {
                    return;
                }
                std::shared_ptr<LegacyControllerMapping> pMapping = load(pConfig, path);
                if (!pMapping) {
                    // No mapping (none remembered, or its file is gone):
                    // on with nothing in it, so what it sends can be learned.
                    pMapping = std::make_shared<LegacyMidiControllerMapping>();
                    pMapping->setFilePath(path);
                }
                qInfo() << "B2B: the mapping wizard switches on" << pController->getName();
                pManager->slotApplyMapping(pController, pMapping, true);
                pConfig->save();
            },
            Qt::QueuedConnection);
}

bool backToOriginal(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController) {
    const Files f = files(pConfig, pManager, pController);
    if (!f.yours) {
        return false;
    }
    const auto pOriginal = load(pConfig, f.original);
    if (!pOriginal) {
        // Made from no mapping: back to none.
        QMetaObject::invokeMethod(
                pManager,
                [pConfig, pManager, pController]() {
                    pManager->slotApplyMapping(pController, nullptr, false);
                    pConfig->save();
                },
                Qt::QueuedConnection);
        emit Notifier::instance()->mappingsChanged();
        return true;
    }
    pOriginal->setDirty(false);
    apply(pConfig, pManager, pController, pOriginal);
    return true;
}

void setInRoom(bool inRoom) {
    s_inRoom.store(inRoom, std::memory_order_release);
}

bool inRoom() {
    return s_inRoom.load(std::memory_order_acquire);
}

Notifier* Notifier::instance() {
    static Notifier* s_pNotifier = new Notifier();
    return s_pNotifier;
}

} // namespace mixxx::b2b::mapwizard
