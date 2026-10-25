#include "b2b/b2bboothcontrols.h"

#include <QApplication>
#include <algorithm>
#include <QHeaderView>
#include <QInputDialog>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <cmath>
#include <QTableView>
#include <QTimer>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "b2b/b2bexplorer.h"
#include "b2b/b2btemporange.h"
#include "library/library_decl.h"
#include "widget/wlibrarysidebar.h"

namespace mixxx::b2b {

namespace {
const QString kGroup = QStringLiteral("[B2BBooth]");

ConfigKey key(int deck, const char* what) {
    return ConfigKey(kGroup, QStringLiteral("deck%1_%2").arg(deck).arg(QLatin1String(what)));
}
} // namespace

BoothControls::BoothControls(int decks, QObject* pParent)
        : QObject(pParent) {
    m_decks.resize(decks);
    QTimer::singleShot(0, this, &BoothControls::startRanges);
    m_xfCurve = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("xf_curve")));
    m_xfCurve->setButtonMode(ControlPushButton::TOGGLE);
    m_xfCurve->setStates(3);
    connect(m_xfCurve.get(), &ControlObject::valueChanged, this, [this](double v) {
        applyXfCurve(static_cast<int>(v));
    });
    m_chatOpen = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("chat_open")), true, 1.0);
    m_chatOpen->setButtonMode(ControlPushButton::TOGGLE);
    // Four decks: the room's panels hidden (1) by their header bar.
    m_panelsOpen = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("panels_open")));
    m_panelsOpen->setButtonMode(ControlPushButton::TOGGLE);
    if (qApp) {
        qApp->installEventFilter(this); // double-click on a BPM readout
    }
    m_trackTimer.setInterval(1000);
    connect(&m_trackTimer, &QTimer::timeout, this, &BoothControls::watchTracks);
    m_trackTimer.start();
    m_columnTimer.setInterval(600);
    connect(&m_columnTimer, &QTimer::timeout, this, &BoothControls::fitColumns);
    for (int side = 0; side < 2; ++side) {
        m_sidePage[side] = std::make_unique<ControlObject>(
                ConfigKey(kGroup, side == 0 ? QStringLiteral("left_page") : QStringLiteral("right_page")));
        // The player's stack writes the page too (when it is first shown).
        connect(m_sidePage[side].get(), &ControlObject::valueChanged, this, [this, side](double) {
            sideChanged(side);
        });
    }
    for (int i = 0; i < decks; ++i) {
        const int n = i + 1;
        Deck& d = m_decks[i];
        d.showView = std::make_unique<ControlPushButton>(key(n, "show_view"));
        d.showBrowse = std::make_unique<ControlPushButton>(key(n, "show_browse"));
        d.back = std::make_unique<ControlPushButton>(key(n, "back"));
        d.listUp = std::make_unique<ControlPushButton>(key(n, "list_up"));
        d.listDown = std::make_unique<ControlPushButton>(key(n, "list_down"));
        d.usb = std::make_unique<ControlPushButton>(key(n, "usb"));
        d.peerBrowsing = std::make_unique<ControlObject>(key(n, "peer_browsing"));
        d.page = std::make_unique<ControlObject>(key(n, "page"));
        d.viewing = std::make_unique<ControlObject>(key(n, "viewing"));
        d.browsing = std::make_unique<ControlObject>(key(n, "browsing"));
        d.viewing->forceSet(0.0);
        // The button: each press, the next range.
        d.range = std::make_unique<ControlPushButton>(key(n, "range"));
        d.rangeIndex = std::make_unique<ControlObject>(key(n, "tempo_range"));
        d.rangeIndex->forceSet(tempo::kFirst);
        connect(d.range.get(), &ControlObject::valueChanged, this, [this, i, n](double v) {
            if (v <= 0) {
                return;
            }
            const int count = static_cast<int>(tempo::kRanges.size());
            applyRange(n, (static_cast<int>(m_decks[i].rangeIndex->get()) + 1) % count);
        });
        d.grid = std::make_unique<ControlPushButton>(key(n, "grid"));
        d.grid->setButtonMode(ControlPushButton::TOGGLE);
        d.tagList = std::make_unique<ControlPushButton>(key(n, "tag_list"));
        d.playlist = std::make_unique<ControlPushButton>(key(n, "playlist"));
        connect(d.tagList.get(), &ControlObject::valueChanged, this, [this, i](double v) {
            if (v > 0) {
                browseTo(i, QStringLiteral("Auto DJ"));
            }
        });
        connect(d.playlist.get(), &ControlObject::valueChanged, this, [this, i](double v) {
            if (v > 0) {
                browseTo(i, QStringLiteral("Playlists"));
            }
        });
        d.standby = std::make_unique<ControlObject>(key(n, "standby"));
        d.standby->forceSet(1.0);
        d.powering = std::make_unique<ControlObject>(key(n, "powering"));
        d.powering->connectValueChangeRequest(this, [this, i](double v) {
            m_decks[i].powering->setAndConfirm(v);
            updateScreen(i);
        });
        d.select = std::make_unique<ControlPushButton>(key(n, "select"));
        d.selected = std::make_unique<ControlObject>(key(n, "selected"));
        d.selected->forceSet(i < 2 ? 1.0 : 0.0);
        d.select->connectValueChangeRequest(this, [this, i](double v) {
            if (v > 0) {
                selectDeck(i);
            }
        });
        d.showView->connectValueChangeRequest(this, [this, i](double v) {
            if (v > 0) {
                setPage(i, 0);
            }
        });
        d.showBrowse->connectValueChangeRequest(this, [this, i](double v) {
            if (v > 0) {
                // Pressing BROWSE again goes back, like a toggle.
                setPage(i, m_decks[i].page->get() > 0 ? 0 : 1);
            }
        });
        // BACK: a step back in the music browser; at the start, close it.
        d.back->connectValueChangeRequest(this, [this, i](double v) {
            if (v > 0 && m_decks[i].page->get() > 0 && !LibraryExplorer::goBack()) {
                setPage(i, 0);
            }
        });
        d.listUp->connectValueChangeRequest(this, [this, i](double v) {
            if (v > 0 && m_decks[i].page->get() > 0) {
                LibraryExplorer::moveSelection(-1);
            }
        });
        d.listDown->connectValueChangeRequest(this, [this, i](double v) {
            if (v > 0 && m_decks[i].page->get() > 0) {
                LibraryExplorer::moveSelection(1);
            }
        });
        connect(d.peerBrowsing.get(), &ControlObject::valueChanged, this, [this, i](double) {
            updateScreen(i);
        });
        // A USB slot: the browser on this player, on the USB drive.
        d.usb->connectValueChangeRequest(this, [this, i](double v) {
            if (v > 0) {
                setPage(i, 1);
                QTimer::singleShot(0, this, [] { LibraryExplorer::openUsb(); });
            }
        });
        // The stack writes the page too (when it is first shown).
        connect(d.page.get(), &ControlObject::valueChanged, this, [this, i](double) {
            pageChanged(i);
        });
    }
}

BoothControls::~BoothControls() {
    if (qApp) {
        qApp->removeEventFilter(this);
    }
}

bool BoothControls::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (pEvent->type() == QEvent::MouseButtonDblClick) {
        const QString name = pObject->objectName();
        // The Booth's "ScreenBpmN", or a skin's plain "BpmN".
        static const QRegularExpression kBpmName(QStringLiteral("^(Screen)?Bpm([1-4])$"));
        const QRegularExpressionMatch match = kBpmName.match(name);
        if (match.hasMatch()) {
            const int deck = match.captured(2).toInt();
            if (deck >= 1 && deck <= static_cast<int>(m_decks.size())) {
                // Open the prompt once this click has been handled.
                QPointer<QWidget> pNear = qobject_cast<QWidget*>(pObject);
                QTimer::singleShot(0, this, [this, deck, pNear] { askForBpm(deck, pNear.data()); });
                return true;
            }
        }
    }
    return QObject::eventFilter(pObject, pEvent);
}

void BoothControls::askForBpm(int deck, QWidget* pNear) {
    const QString group = QStringLiteral("[Channel%1]").arg(deck);
    double fileBpm = ControlObject::get(ConfigKey(group, QStringLiteral("file_bpm")));
    if (fileBpm <= 0) {
        // file_bpm can be 0 for a track with a grid (e.g. one loaded from the
        // library with its saved grid): the shown BPM without the tempo fader.
        const double ratio = ControlObject::get(ConfigKey(group, QStringLiteral("rate_ratio")));
        const double shown = ControlObject::get(ConfigKey(group, QStringLiteral("bpm")));
        fileBpm = ratio > 0 ? shown / ratio : 0;
    }
    if (fileBpm <= 0 || !pNear || !pNear->window()) {
        return; // no track, or no beat grid yet
    }
    const double current = ControlObject::get(ConfigKey(group, QStringLiteral("bpm")));
    bool ok = false;
    const double bpm = QInputDialog::getDouble(pNear->window(),
            tr("Set the tempo"),
            tr("Deck %1 BPM (the track is %2):").arg(deck).arg(fileBpm, 0, 'f', 2),
            current > 0 ? current : fileBpm,
            20.0,
            400.0,
            2,
            &ok);
    if (!ok) {
        return;
    }
    // Widen the tempo range if the fader can't reach this BPM.
    const double needed = std::fabs(bpm / fileBpm - 1.0);
    const double range = ControlObject::get(ConfigKey(group, QStringLiteral("rateRange")));
    if (needed > range) {
        const int last = static_cast<int>(tempo::kRanges.size()) - 1;
        for (int i = 0; i <= last; ++i) {
            if (tempo::kRanges[i].span >= needed || i == last) {
                applyRange(deck, i);
                break;
            }
        }
    }
    ControlObject::set(ConfigKey(group, QStringLiteral("bpm")), bpm);
}

void BoothControls::applyRange(int n, int i) {
    i = std::clamp(i, 0, static_cast<int>(tempo::kRanges.size()) - 1);
    const QString group = QStringLiteral("[Channel%1]").arg(n);
    const ConfigKey rangeKey(group, QStringLiteral("rateRange"));
    const ConfigKey faderKey(group, QStringLiteral("rate"));
    if (!ControlObject::exists(rangeKey)) {
        return;
    }
    // As a CDJ: the fader stays put, so the speed changes with the range
    // (Mixxx would keep the speed and move the fader).
    const double fader = ControlObject::get(faderKey);
    ControlObject::set(rangeKey, tempo::kRanges[i].span);
    ControlObject::set(faderKey, fader);
    m_decks[n - 1].rangeIndex->forceSet(i);
}

void BoothControls::startRanges() {
    if (!ControlObject::exists(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("rateRange")))) {
        QTimer::singleShot(500, this, &BoothControls::startRanges); // decks not made yet
        return;
    }
    for (int i = 0; i < static_cast<int>(m_decks.size()); ++i) {
        Deck& d = m_decks[i];
        const int n = i + 1;
        const QString group = QStringLiteral("[Channel%1]").arg(n);
        if (!ControlObject::exists(ConfigKey(group, QStringLiteral("rateRange")))) {
            continue;
        }
        applyRange(n, tempo::kFirst);
        // A range set any other way (a friend's deck in a session): the
        // button and the screen follow it.
        d.rateRange = std::make_unique<ControlProxy>(group, QStringLiteral("rateRange"), this);
        d.rateRange->connectValueChanged(this, [this, i, n](double span) {
            Deck& deck = m_decks[i];
            const int index = tempo::indexOf(span);
            if (std::fabs(tempo::kRanges[index].span - span) > 1e-6) {
                // Not a CDJ range (Mixxx's own ±8 from its settings): the
                // nearest one.
                applyRange(n, index);
                return;
            }
            if (deck.rangeIndex->get() != index) {
                deck.rangeIndex->forceSet(index);
            }
        });
    }
}

void BoothControls::applyXfCurve(int position) {
    // Mixxx's crossfader: mode 0 = additive, 1 = constant power; a higher
    // curve value makes each side come in faster.
    static const double kCurve[3] = {1.0, 3.0, 100.0};
    static const double kMode[3] = {1.0, 0.0, 0.0};
    position = std::clamp(position, 0, 2);
    const QString group = QStringLiteral("[Mixer Profile]");
    ControlObject::set(ConfigKey(group, QStringLiteral("xFaderMode")), kMode[position]);
    ControlObject::set(ConfigKey(group, QStringLiteral("xFaderCurve")), kCurve[position]);
    // The calibration goes with the curve (as Preferences -> Mixer sets it);
    // without it the constant-power curve is lopsided.
    ControlObject::set(ConfigKey(group, QStringLiteral("xFaderCalibration")),
            std::pow(0.5, 1.0 / kCurve[position]));
}

// Deck index i (0-based): side i % 2, page i / 2 on that side.
void BoothControls::selectDeck(int deck) {
    const int side = deck % 2;
    if (side >= 2 || !m_sidePage[side]) {
        return;
    }
    const double page = deck / 2;
    auto apply = [this, side, page] {
        m_sidePage[side]->set(page);
        sideChanged(side); // set() by us does not emit valueChanged to us
    };
    if (m_flipHook && m_sidePage[side]->get() != page) {
        m_flipHook(side, apply);
    } else {
        apply();
    }
}

void BoothControls::resetPlayers() {
    for (int i = 0; i < static_cast<int>(m_decks.size()); ++i) {
        applyRange(i + 1, tempo::kFirst);
        if (m_decks[i].page && m_decks[i].page->get() > 0) {
            setPage(i, 0);
        }
    }
}

int BoothControls::shownDeck(int side) const {
    if (side < 0 || side >= 2 || !m_sidePage[side]) {
        return side;
    }
    return static_cast<int>(m_sidePage[side]->get()) * 2 + side;
}

void BoothControls::sideChanged(int side) {
    const int shown = static_cast<int>(m_sidePage[side]->get()) * 2 + side;
    for (int i = side; i < static_cast<int>(m_decks.size()); i += 2) {
        m_decks[i].selected->set(i == shown ? 1.0 : 0.0);
        // A deck that leaves the screen stops browsing.
        if (i != shown && m_decks[i].page->get() > 0) {
            m_decks[i].page->set(0.0);
            pageChanged(i);
        }
    }
}

void BoothControls::setPage(int deck, int page) {
    if (page == 1) {
        for (int j = 0; j < static_cast<int>(m_decks.size()); ++j) {
            if (j != deck && m_decks[j].page->get() > 0) {
                m_decks[j].page->set(0.0);
                pageChanged(j);
            }
        }
    }
    m_decks[deck].page->set(page);
    pageChanged(deck); // set() by us does not emit valueChanged to us
    if (page == 1) {
        // Put the keyboard and wheel on the track list once it is on screen.
        QTimer::singleShot(0, this, [this] {
            if (!m_focusedWidget) {
                m_focusedWidget = std::make_unique<ControlProxy>(
                        ConfigKey(QStringLiteral("[Library]"), QStringLiteral("focused_widget")));
            }
            if (m_focusedWidget->valid()) {
                m_focusedWidget->set(static_cast<double>(FocusWidget::TracksTable));
            }
        });
    }
}

void BoothControls::watchTracks() {
    // Decks 3 and 4 appear later (when a skin asks for four decks).
    for (int i = 0; i < static_cast<int>(m_decks.size()); ++i) {
        Deck& d = m_decks[i];
        if (d.trackLoaded && d.trackLoaded->valid()) {
            continue;
        }
        const ConfigKey key(QStringLiteral("[Channel%1]").arg(i + 1), QStringLiteral("track_loaded"));
        if (!ControlObject::exists(key)) {
            continue;
        }
        d.trackLoaded = std::make_unique<ControlProxy>(key, this, ControlFlag::NoWarnIfMissing);
        d.lastLoaded = d.trackLoaded->get();
        d.trackLoaded->connectValueChanged(this, [this, i](double v) {
            Deck& deck = m_decks[i];
            const bool cameIn = deck.lastLoaded <= 0 && v > 0;
            deck.lastLoaded = v;
            if (cameIn && deck.page->get() <= 0) {
                // The screen powers up before showing the deck (the skin's
                // standby screen clears this; if there is none, so do we).
                deck.powering->set(1.0);
                QTimer::singleShot(1500, this, [this, i] {
                    m_decks[i].powering->set(0.0);
                    updateScreen(i);
                });
            }
            updateScreen(i);
        });
        updateScreen(i);
    }
}

void BoothControls::updateScreen(int deck) {
    Deck& d = m_decks[deck];
    const bool browsing = d.page->get() > 0;
    const bool loaded = d.trackLoaded && d.trackLoaded->get() > 0;
    const bool powering = d.powering->get() > 0;
    // The friend's browser shown here instead (only when we aren't browsing).
    const bool peer = !browsing && d.peerBrowsing && d.peerBrowsing->get() > 0;
    // The deck (waveform, readouts) only with a track, once powered up.
    d.viewing->set(!browsing && !peer && loaded && !powering ? 1.0 : 0.0);
    d.standby->set(!browsing && !peer && (!loaded || powering) ? 1.0 : 0.0);
}

void BoothControls::pageChanged(int deck) {
    const bool browsing = m_decks[deck].page->get() > 0;
    m_decks[deck].browsing->set(browsing ? 1.0 : 0.0);
    updateScreen(deck);
    bool anyBrowsing = false;
    for (const Deck& d : m_decks) {
        anyBrowsing = anyBrowsing || d.page->get() > 0;
    }
    if (anyBrowsing) {
        // (The B2B music browser sets the track list's columns itself.)
    } else {
        m_columnTimer.stop();
    }
}

namespace {
bool insideBrowser(const QWidget* pWidget) {
    for (; pWidget; pWidget = pWidget->parentWidget()) {
        if (pWidget->objectName() == QLatin1String("BoothBrowser")) {
            return true;
        }
    }
    return false;
}
} // namespace

void BoothControls::fitColumns() {
    // Header names as the track models give them, in the order to show them,
    // with a width each (the list stretches the last one).
    static const QList<QPair<QString, int>> kWanted = {
            {QStringLiteral("Title"), 118},
            {QStringLiteral("Artist"), 84},
            {QStringLiteral("BPM"), 62}, // with its lock box
            {QStringLiteral("Key"), 36},
            {QStringLiteral("Duration"), 48},
    };
    const auto windows = QApplication::topLevelWidgets();
    for (QWidget* pWindow : windows) {
        const auto views = pWindow->findChildren<QTableView*>();
        for (QTableView* pView : views) {
            if (!pView->isVisible() || !insideBrowser(pView) || !pView->model()) {
                continue;
            }
            QHeaderView* pHeader = pView->horizontalHeader();
            const QAbstractItemModel* pModel = pView->model();
            QHash<QString, int> byName;
            for (int c = 0; c < pModel->columnCount(); ++c) {
                byName.insert(pModel->headerData(c, Qt::Horizontal).toString(), c);
            }
            int found = 0;
            for (const auto& want : kWanted) {
                found += byName.contains(want.first) ? 1 : 0;
            }
            if (found < 2) {
                continue; // not a track list (e.g. a feature's own view)
            }
            // Already fitted?
            bool fitted = true;
            int visual = 0;
            for (const auto& want : kWanted) {
                auto it = byName.constFind(want.first);
                if (it == byName.constEnd()) {
                    continue;
                }
                if (pHeader->isSectionHidden(*it) || pHeader->visualIndex(*it) != visual++) {
                    fitted = false;
                }
            }
            for (int c = 0; c < pModel->columnCount() && fitted; ++c) {
                const bool wanted = std::any_of(kWanted.begin(), kWanted.end(), [&](const auto& w) {
                    return byName.value(w.first, -1) == c;
                });
                fitted = wanted || pHeader->isSectionHidden(c);
            }
            if (fitted) {
                continue;
            }
            for (int c = 0; c < pModel->columnCount(); ++c) {
                pHeader->setSectionHidden(c, true);
            }
            visual = 0;
            for (const auto& want : kWanted) {
                auto it = byName.constFind(want.first);
                if (it == byName.constEnd()) {
                    continue;
                }
                pHeader->setSectionHidden(*it, false);
                pHeader->moveSection(pHeader->visualIndex(*it), visual++);
                pHeader->resizeSection(*it, want.second);
            }
            pHeader->setStretchLastSection(true);
        }
    }
}

void BoothControls::browseTo(int deck, const QString& sidebarItem) {
    if (m_decks[deck].page->get() <= 0) {
        setPage(deck, 1);
    }
    QTimer::singleShot(50, this, [this, sidebarItem] {
        const auto windows = QApplication::topLevelWidgets();
        for (QWidget* pWindow : windows) {
            const auto sidebars = pWindow->findChildren<WLibrarySidebar*>();
            for (WLibrarySidebar* pSidebar : sidebars) {
                QAbstractItemModel* pModel = pSidebar->model();
                if (!pModel || !insideBrowser(pSidebar)) {
                    continue;
                }
                for (int r = 0; r < pModel->rowCount(); ++r) {
                    const QModelIndex index = pModel->index(r, 0);
                    if (index.data().toString() == sidebarItem) {
                        pSidebar->selectIndex(index);
                        emit pSidebar->pressed(index);
                        return;
                    }
                }
            }
        }
    });
}

} // namespace mixxx::b2b

#include "moc_b2bboothcontrols.cpp"
