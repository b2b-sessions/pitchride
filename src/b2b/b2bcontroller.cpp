#include "b2b/b2bcontroller.h"

#include "mixer/deck.h"
#include "mixer/playermanager.h"
#include "track/track.h"

#include <QMenu>

#include "b2b/b2bcontrollerrouting.h"
#include "b2b/b2bdeckflip.h"
#include "b2b/b2bmappingwizard.h"
#include "b2b/b2btheme.h"
#include "b2b/b2broomchat.h"
#include "b2b/b2broompanels.h"
#include "b2b/b2broomspage.h"
#include "b2b/b2broomstrip.h"
#include "b2b/b2btopbar.h"

#include "b2b/b2bcrashreport.h"
#include "b2b/b2bmanager.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QMainWindow>
#include <QMenuBar>

#include "widget/wmainmenubar.h"
#include <QUrl>
#include <QMessageBox>
#include <QSettings>
#include <QEventLoop>
#include <QTimer>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QTextStream>
#include <QPushButton>
#include <QShortcut>
#include <QStandardPaths>
#include <QToolBar>

#include <QUrlQuery>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "library/library_prefs.h"
#include "track/keyutils.h"
#include "control/controlproxy.h"
#include "b2b/report.hpp"
#include "b2b/session.hpp"
#include "b2b/version.hpp"
#include "skin/legacy/legacyskinparser.h"
#include "util/logging.h"
#include "util/versionstore.h"


namespace mixxx::b2b {

namespace {
// (The menus look as the top bar's: theme::menu().)

/// A question with its answers in words ("close room" / "keep it open"),
/// not yes and no. True: the first.
bool confirm(QWidget* pParent, const QString& title, const QString& text, const QString& yes, const QString& no) {
    QMessageBox box(QMessageBox::NoIcon, title, text, QMessageBox::NoButton, pParent);
    QPushButton* pYes = box.addButton(yes, QMessageBox::AcceptRole);
    box.addButton(no, QMessageBox::RejectRole);
    box.setDefaultButton(pYes);
    box.exec();
    return box.clickedButton() == pYes;
}
} // namespace

Controller::Controller(PlayerManager* pPlayerManager,
        EngineMixer* pEngineMixer,
        std::shared_ptr<EffectsManager> pEffectsManager,
        const QString& settingsPath,
        QObject* pParent)
        : QObject(pParent),
          m_pPlayerManager(pPlayerManager),
          m_pEngineMixer(pEngineMixer),
          m_pEffectsManager(std::move(pEffectsManager)),
          m_settingsPath(settingsPath),
          m_pUi(std::make_unique<::b2b::qt::SessionUi>(
                  QDir(settingsPath).filePath(QStringLiteral("b2b.ini")))),
          m_nowPlaying(pPlayerManager),
          m_site(settingsPath) {
    m_mixer.setEffectsManager(m_pEffectsManager);
    m_mixer.setSettingsPath(settingsPath);
    connect(m_pUi.get(), &::b2b::qt::SessionUi::startRequested, this, &Controller::start);
    connect(m_pUi.get(), &::b2b::qt::SessionUi::leaveRequested, this, &Controller::leaveClicked);
    connect(m_pUi.get(), &::b2b::qt::SessionUi::reportRequested, this, &Controller::sendReport);
    // Closing the app closes your room (or leaves the one you're in), so
    // nobody is left trying to reach an app that has gone.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &Controller::leaveRoomsOnQuit);
    if (Manager::enabledByEnvironment()) {
        startFromEnvironment();
    }
    // 2 or 4 decks on screen: before the first skin loads.
    LegacySkinParser::setFourDecks(QSettings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")),
            QSettings::IniFormat)
                                           .value(QStringLiteral("deck_view"), 2)
                                           .toInt() == 4);
    // The booth at its own size: the two-deck panels are dragged up over
    // it now, not laid out under a smaller one (the old panels_px is not
    // used; their height over it is panels_over).
    LegacySkinParser::setPanelsHeight(0);
    m_explorer.setSettingsPath(m_settingsPath);
    m_deckViewTimer.setInterval(1500);
    connect(&m_deckViewTimer, &QTimer::timeout, this, &Controller::updateDeckViewSwitch);
    // The website opens pitchride:// links.
    links::registerScheme();
    connect(&m_links, &LinkServer::linkReceived, this, &Controller::openLink);
}

Controller::~Controller() {
    leave();
    if (m_reportThread.joinable()) {
        m_reportThread.join(); // a report still sending (gives up after 20 s)
    }
}

QString Controller::reportServer() const {
    // Same place as the join codes: B2B_SERVER, else b2b.ini "server=", else ours.
    QString server = qEnvironmentVariable("B2B_SERVER");
    if (server.isEmpty()) {
        server = QSettings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat)
                         .value(QStringLiteral("server"))
                         .toString()
                         .trimmed();
    }
    if (server.isEmpty()) {
        server = QString::fromLatin1(::b2b::defaultRendezvousServer());
    }
    return server;
}

namespace {

// The end of the log: the most recent part is what matters.
QString logTail(const QString& path, qint64 maxBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const qint64 size = file.size();
    if (size > maxBytes) {
        file.seek(size - maxBytes);
        file.readLine(); // start on a whole line
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

void Controller::sendReport(const ::b2b::qt::ReportRequest& request) {
    QJsonObject report;
    report[QStringLiteral("kind")] = request.logsOnly ? QStringLiteral("logs") : QStringLiteral("bug");
    report[QStringLiteral("message")] = request.message;
    report[QStringLiteral("contact")] = request.contact;
    if (request.includeLog) {
        mixxx::Logging::flushLogFile();
        report[QStringLiteral("log")] = logTail(QDir(m_settingsPath).filePath(QStringLiteral("mixxx.log")),
                2 * 1024 * 1024);
    }
    sendReportJson(std::move(report));
}

void Controller::sendReportJson(QJsonObject report) {
    if (m_reportThread.joinable()) {
        m_reportThread.join(); // the previous one has finished (the UI allows one at a time)
    }
    ::b2b::crypto::Key key{};
    const QString keyHex = qEnvironmentVariable("B2B_REPORT_KEY", QString::fromLatin1(::b2b::kReportPublicKeyHex));
    if (!::b2b::crypto::fromHex(keyHex.toStdString(), key)) {
        m_pUi->reportFinished(false, QString(), tr("this version of the app can't send reports."));
        return;
    }
    const QString kind = report.value(QStringLiteral("kind")).toString();
    report[QStringLiteral("app")] = QString::fromLatin1(::b2b::kVersion);
    report[QStringLiteral("mixxx")] = VersionStore::version();
    report[QStringLiteral("os")] = QString(QSysInfo::prettyProductName() + QStringLiteral(" (") +
            QSysInfo::currentCpuArchitecture() + QLatin1Char(')'));
    report[QStringLiteral("time")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!report.contains(QStringLiteral("session"))) {
        report[QStringLiteral("session")] =
                m_pManager ? QStringLiteral("in a session") : QStringLiteral("not in a session");
    }
    // "B2BR" + format 1 (Qt-compressed JSON), then sealed for the developers.
    QByteArray plain("B2BR\x01", 5);
    plain += qCompress(QJsonDocument(report).toJson(QJsonDocument::Compact), 9);
    std::vector<uint8_t> sealed = ::b2b::sealReport(key, reinterpret_cast<const uint8_t*>(plain.constData()),
            static_cast<size_t>(plain.size()));

    const QString server = reportServer();
    const int colon = server.lastIndexOf(QLatin1Char(':'));
    const std::string host = (colon > 0 ? server.left(colon) : server).toStdString();
    const auto port = static_cast<uint16_t>(colon > 0 ? server.mid(colon + 1).toUInt() : 7700);
    qInfo() << "B2B: sending a" << kind << "report to" << server << "(" << sealed.size() / 1024 << "KB)";
    m_reportThread = std::thread([this, host, port, sealed = std::move(sealed)] {
        const ::b2b::ReportResult result = ::b2b::sendReport(host, port, sealed);
        QMetaObject::invokeMethod(this, [this, result] {
            if (result.ok) {
                qInfo() << "B2B: report sent, reference" << QString::fromStdString(result.reference);
            } else {
                qWarning() << "B2B: report not sent:" << QString::fromStdString(result.error);
            }
            m_pUi->reportFinished(result.ok,
                    QString::fromStdString(result.reference),
                    QString::fromStdString(result.error));
        }, Qt::QueuedConnection);
    });
}

void Controller::checkForCrashReports() {
    const QStringList reports = unseenCrashReports(m_settingsPath);
    if (reports.isEmpty()) {
        return;
    }
    markCrashReportsSeen(m_settingsPath);
    const QString path = reports.last();
    // The log of the run that crashed is mixxx.log.1 now (each start moves
    // the logs along). Its end goes in the report: the last lines first (what
    // was happening), then more of it.
    const QString previousLog = logTail(QDir(m_settingsPath).filePath(QStringLiteral("mixxx.log.1")),
            1024 * 1024);
    {
        QFile file(path);
        if (file.open(QIODevice::Append | QIODevice::Text)) {
            QStringList lines = previousLog.split(QLatin1Char('\n'));
            while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
                lines.removeLast();
            }
            const QStringList last = lines.mid(std::max(0, static_cast<int>(lines.size()) - 40));
            QTextStream out(&file);
            out << "\n---- The last lines of the log before the crash ----\n"
                << last.join(QLatin1Char('\n'))
                << "\n\n---- More of the log (mixxx.log.1) ----\n"
                << previousLog;
        }
    }
    qWarning() << "B2B: the last run crashed, report" << path;

    QMessageBox box(m_pWindow);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("PitchRide closed unexpectedly"));
    const QString reportName = QFileInfo(path).completeBaseName();
    box.setText(reportName.endsWith(QStringLiteral("-freeze"))
                    ? tr("the app froze last time and was closed. a report was saved.")
                    : reportName.endsWith(QStringLiteral("-ended"))
                    ? tr("the app didn't close properly last time. a report was saved.")
                    : tr("the app crashed last time. a report was saved."));
    box.setInformativeText(tr("send it to the developers, or open the folder and send the file "
                              "(%1) to whoever is fixing it.")
                                   .arg(QFileInfo(path).fileName()));
    // (No details in the box: on a Mac they showed in full and pushed the
    // buttons off a laptop's screen. The report is in the folder.)
    QPushButton* pSend = box.addButton(tr("Send to the developers"), QMessageBox::AcceptRole);
    QPushButton* pOpen = box.addButton(tr("Open the folder"), QMessageBox::ActionRole);
    box.addButton(tr("Close"), QMessageBox::RejectRole);
    box.setDefaultButton(pSend);
    box.exec();
    if (box.clickedButton() == pOpen) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(crashReportFolder(m_settingsPath)));
    } else if (box.clickedButton() == pSend) {
        QFile full(path);
        QString crash;
        if (full.open(QIODevice::ReadOnly | QIODevice::Text)) {
            crash = QString::fromUtf8(full.read(64 * 1024));
        }
        QJsonObject report;
        report[QStringLiteral("kind")] = QStringLiteral("crash");
        report[QStringLiteral("message")] = crash;
        report[QStringLiteral("contact")] = QString();
        report[QStringLiteral("session")] = QStringLiteral("(the run that crashed)");
        report[QStringLiteral("log")] = previousLog;
        sendReportJson(std::move(report));
    }
}

void Controller::setControllerManager(std::shared_ptr<ControllerManager> pManager) {
    m_mappingWizard.setControllerManager(std::move(pManager), m_pConfig);
    // Settings > Mapping wizard > Start: the booth, with the wizard's strip.
    connect(mapwizard::Notifier::instance(),
            &mapwizard::Notifier::startRequested,
            this,
            [this](::Controller* pController) {
                if (inRoom()) {
                    return;
                }
                showTab(TopBar::Booth);
                m_mappingWizard.start(pController);
            });
}

void Controller::setConfig(UserSettingsPointer pConfig) {
    m_pConfig = std::move(pConfig);
    if (m_pKeyNumbers) {
        return;
    }
    m_pKeyNumbers = std::make_unique<ControlPushButton>(ConfigKey(QStringLiteral("[B2B]"), QStringLiteral("key_numbers")));
    m_pKeyNumbers->setButtonMode(ControlPushButton::TOGGLE);
    const QString notation = m_pConfig->getValue(ConfigKey(QStringLiteral("[Key]"), QStringLiteral("KeyNotation")));
    m_pKeyNumbers->set(notation.startsWith(QStringLiteral("Lancelot")) || notation.startsWith(QStringLiteral("OpenKey"))
                    ? 1.0
                    : 0.0);
    connect(m_pKeyNumbers.get(), &ControlObject::valueChanged, this, [this](double value) {
        applyKeyNumbers(value > 0);
    });
}

void Controller::applyKeyNumbers(bool numbers) {
    qInfo() << "B2B: keys shown as" << (numbers ? "numbers" : "names");
    // Numbers like a CDJ's (8A), or the key's name (Am); kept in Mixxx's own
    // key setting, so the library and Preferences agree.
    const KeyUtils::KeyNotation type = numbers ? KeyUtils::KeyNotation::Lancelot : KeyUtils::KeyNotation::Traditional;
    QMap<mixxx::track::io::key::ChromaticKey, QString> names;
    for (int k = mixxx::track::io::key::C_MAJOR; k <= mixxx::track::io::key::B_MINOR; ++k) {
        const auto key = static_cast<mixxx::track::io::key::ChromaticKey>(k);
        names[key] = KeyUtils::keyToString(key, type);
    }
    KeyUtils::setNotation(names);
    if (m_pConfig) {
        m_pConfig->setValue(ConfigKey(QStringLiteral("[Key]"), QStringLiteral("KeyNotation")),
                numbers ? QStringLiteral("Lancelot") : QStringLiteral("Traditional"));
    }
    // Everything showing a key redraws.
    ControlObject::set(mixxx::library::prefs::kKeyNotationConfigKey, static_cast<double>(type));
}

void Controller::openCrashReports() {
    const QString folder = crashReportFolder(m_settingsPath);
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void Controller::setTrackCollectionManager(std::shared_ptr<TrackCollectionManager> pTrackCollectionManager) {
    m_pTrackCollectionManager = std::move(pTrackCollectionManager);
    if (m_pManager) {
        m_pManager->setTrackCollectionManager(m_pTrackCollectionManager);
    }
}

void Controller::setLibrary(Library* pLibrary) {
    m_explorer.setLibrary(pLibrary);
}

void Controller::installInto(QMainWindow* pWindow) {
    if (!pWindow) {
        return;
    }
    m_pUi->setDialogParent(pWindow);
    m_pointer.setWindow(pWindow);
    m_peerBrowser.setWindow(pWindow);
    m_deckScreens.setWindow(pWindow);
    m_mixer.setWindow(pWindow);
    m_explorer.setWindow(pWindow);
    m_owners.setWindow(pWindow);
    m_flipTabs.setWindow(pWindow);
    m_nowPlaying.setWindow(pWindow);
    m_aboutGear.setWindow(pWindow);
    m_loopReadout.setWindow(pWindow);
    if (!m_menuReady) {
        m_menuReady = true;
        m_pUi->menu()->addSeparator();
        // Each DJ sees the other's pointer on the decks.
        QAction* pPointer = m_pUi->menu()->addAction(tr("Show my friend's pointer (and mine to them)"));
        pPointer->setCheckable(true);
        const bool pointerOn = QSettings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")),
                QSettings::IniFormat)
                                       .value(QStringLiteral("share_pointer"), true)
                                       .toBool();
        pPointer->setChecked(pointerOn);
        m_pointer.setEnabled(pointerOn);
        connect(pPointer, &QAction::toggled, this, [this](bool on) {
            m_pointer.setEnabled(on);
            QSettings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat)
                    .setValue(QStringLiteral("share_pointer"), on);
        });
        m_pUi->menu()->addAction(tr("Open the crash reports folder"), this, &Controller::openCrashReports);
        // The last run crashed? Offer its report once the window is up.
        QTimer::singleShot(2500, this, &Controller::checkForCrashReports);
    }
    installChrome(pWindow);
    m_pWindow = pWindow;
    // Every message box dark, like the rest of the app (Mixxx's own too).
    if (!m_pDialogStyler) {
        m_pDialogStyler = new DialogStyler(this);
        qApp->installEventFilter(m_pDialogStyler);
    }
    // A player turning round to deck 3 / 4: turned over like a card.
    if (!m_pDeckFlip) {
        m_pDeckFlip = new DeckFlip(pWindow);
        m_booth.setFlipHook([this](int side, std::function<void()> apply) {
            if (m_pDeckFlip) {
                m_pDeckFlip->flip(side, std::move(apply));
            } else {
                apply();
            }
        });
    }
    // A link the app was started with: once the window is up.
    QTimer::singleShot(1500, this, [this] {
        for (const QString& link : links::takePending()) {
            openLink(link);
        }
    });
}

void Controller::installChrome(QMainWindow* pWindow) {
    // Mixxx's menus go out of sight (a bar of no height: their keyboard
    // shortcuts still work); PitchRide's bar takes their place.
    WMainMenuBar::b2bKeepOutOfSight(true);
    if (QMenuBar* pMenuBar = pWindow->menuBar()) {
        pMenuBar->setNativeMenuBar(false);
        pMenuBar->setMinimumHeight(0);
        pMenuBar->setFixedHeight(0);
        pMenuBar->setStyleSheet(QStringLiteral("QMenuBar { background: #000000; border: 0; }") +
                theme::menu());
    }
    m_pUi->menu()->setStyleSheet(theme::menu());
    if (m_pTopBar) {
        return;
    }
    // The B2B menu becomes the room's play options: hosting, joining and
    // codes are the room's business now; bugs and logs are in Help.
    for (QAction* pAction : m_pUi->menu()->actions()) {
        const QString text = pAction->text().remove(QLatin1Char('&'));
        if (text.startsWith(QLatin1String("Host a session")) || text.startsWith(QLatin1String("Join a session")) ||
                text.startsWith(QLatin1String("Copy join code")) || text.startsWith(QLatin1String("Leave session")) ||
                text.startsWith(QLatin1String("Report a bug")) || text.startsWith(QLatin1String("Send logs")) ||
                text.startsWith(QLatin1String("Friends and invites")) ||
                text.startsWith(QLatin1String("Open the crash reports"))) {
            pAction->setVisible(false);
        }
    }
    auto* pBar = new TopBar(pWindow);
    m_pTopBar = pBar;
    m_mappingWizard.setWindow(pWindow, pBar);
    auto* pToolBar = new QToolBar(tr("PitchRide"), pWindow);
    pToolBar->setObjectName(QStringLiteral("B2BToolBar"));
    pToolBar->setMovable(false);
    pToolBar->setFloatable(false);
    pToolBar->setContextMenuPolicy(Qt::PreventContextMenu);
    pToolBar->setStyleSheet(QStringLiteral(
            "QToolBar#B2BToolBar { border: 0; padding: 0; margin: 0; spacing: 0; background: #000000; }"));
    pToolBar->addWidget(pBar);
    pWindow->addToolBar(Qt::TopToolBarArea, pToolBar);
    m_pToolBar = pToolBar;
    // Under it, on the booth tab while you're in a room: the room strip.
    pWindow->addToolBarBreak(Qt::TopToolBarArea);
    auto* pStripBar = new QToolBar(tr("The room"), pWindow);
    pStripBar->setObjectName(QStringLiteral("B2BRoomToolBar"));
    pStripBar->setMovable(false);
    pStripBar->setFloatable(false);
    pStripBar->setContextMenuPolicy(Qt::PreventContextMenu);
    pStripBar->setStyleSheet(QStringLiteral(
            "QToolBar#B2BRoomToolBar { border: 0; padding: 0; margin: 0; spacing: 0; background: transparent; }"));
    auto* pRoomStrip = new RoomStrip(pStripBar);
    pStripBar->addWidget(pRoomStrip);
    pWindow->addToolBar(Qt::TopToolBarArea, pStripBar);
    pStripBar->hide();
    m_pStripBar = pStripBar;
    m_pStrip = pRoomStrip;
    connect(pRoomStrip, &RoomStrip::leaveClicked, this, &Controller::leaveRoomClicked);
    connect(pRoomStrip, &RoomStrip::cancelAskClicked, this, &Controller::leavePlaying);
    // Taking turns: as the session's own buttons would.
    connect(pRoomStrip, &RoomStrip::handOverClicked, this, [this] { emit m_pUi->handOverClicked(); });
    connect(pRoomStrip, &RoomStrip::takeControlClicked, this, [this] { emit m_pUi->takeOrRequestClicked(); });
    connect(pRoomStrip, &RoomStrip::styleChosen, this, &Controller::choosePlayStyle);
    connect(pRoomStrip, &RoomStrip::resetClicked, this, &Controller::resetRoomClicked);
    connect(pRoomStrip, &RoomStrip::panelsClicked, this, [] {
        ControlObject::set(ConfigKey(QStringLiteral("[B2BBooth]"), QStringLiteral("chat_open")), 1.0);
    });

    // The booth's own tool (2 or 4 decks) and, in a session, its strip.
    pBar->setBoothTools(makeDeckViewSwitch());
    QWidget* pStrip = m_pUi->statusStrip();
    // Bugs are reported from Help now; hosting and joining from Rooms.
    for (QPushButton* pButton : pStrip->findChildren<QPushButton*>()) {
        if (pButton->text() == tr("Report a bug")) {
            pButton->hide();
            pButton->setMaximumWidth(0);
        }
    }
    // The strip in the bar's colours.
    pStrip->setStyleSheet(theme::bar() +
            QStringLiteral("#B2BStrip { background: transparent; border: 0; }"
                           "#B2BStrip QLabel#B2BActivity, #B2BStrip QLabel#B2BMessage { color: %1; }"
                           "#B2BStrip QLabel#B2BProblem { color: %2; }"
                           "#B2BStrip QPushButton#B2BLive { background: %3; border-color: %3; color: %4; }")
                    .arg(QLatin1String(theme::kTypeMuted),
                            QLatin1String(theme::kLcd),
                            QLatin1String(theme::kAction),
                            QLatin1String(theme::kOnAction)));
    for (QPushButton* pButton : pStrip->findChildren<QPushButton*>()) {
        theme::caps(pButton);
    }
    pBar->setSession(pStrip);
    m_pSessionStrip = pStrip;

    // Help.
    QMenu* pHelp = pBar->helpMenu();
    pHelp->addAction(tr("User manual"), this, [this] {
        QDesktopServices::openUrl(m_site.url(QStringLiteral("/manual")));
    });
    pHelp->addAction(tr("Report a bug or feedback\xE2\x80\xA6"), this, [this] { m_pUi->openReportDialog(); });
    pHelp->addAction(tr("Send logs to the developers…"), this, [this] { m_pUi->confirmSendLogs(); });
    pHelp->addAction(tr("Open the crash reports folder"), this, &Controller::openCrashReports);
    pHelp->addSeparator();
    pHelp->addAction(tr("About PitchRide"), this, [pWindow] {
        QMetaObject::invokeMethod(pWindow, "slotHelpAbout");
    });
    connect(pBar, &TopBar::settingsClicked, this, [pWindow] {
        QMetaObject::invokeMethod(pWindow, "slotOptionsPreferences");
    });
    connect(&m_site, &Site::signedInChanged, this, &Controller::rebuildProfileMenu);
    // Sign in to use the app: signed out (or signing out), back to the way in.
    auto lockToWayIn = [this] {
        if (auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data())) {
            pBar->setSignedOut(!canUse());
        }
        if (!canUse()) {
            showTab(TopBar::Rooms);
        }
    };
    connect(&m_site, &Site::signedInChanged, this, lockToWayIn);
    connect(&m_site, &Site::meChanged, this, lockToWayIn);
    // A newer PitchRide: in the bar; too old to play: only the way to update.
    // Downloaded and checked: Update installs it; otherwise the website.
    connect(pBar, &TopBar::updateClicked, this, &Controller::updateClicked);
    connect(&m_updates, &UpdateCheck::changed, this, [this, lockToWayIn] {
        showUpdates();
        lockToWayIn();
    });
    connect(&m_updater, &Updater::changed, this, &Controller::showUpdates);
    // The update runs once the app has closed (then PitchRide opens again).
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_installOnQuit) {
            m_updater.install();
        }
    });
    connect(&m_site, &Site::meChanged, this, &Controller::rebuildProfileMenu);
    connect(&m_site, &Site::meChanged, this, &Controller::refreshFriendCodes);
    // Your name on your decks' titles (the friend's app shows it too).
    connect(&m_site, &Site::meChanged, this, [this] {
        if (m_pManager) {
            m_pManager->setMyName(m_site.me().value(QStringLiteral("name")).toString());
        }
    });
    connect(&m_site, &Site::wantsFront, this, [pWindow] {
        pWindow->showNormal();
        pWindow->raise();
        pWindow->activateWindow();
    });
    rebuildProfileMenu();
    if (auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data())) {
        // Opened: the friend code's latest (used by someone since?).
        connect(pBar->profileMenu(), &QMenu::aboutToShow, this, &Controller::refreshFriendCodes);
    }

    // The Rooms page, over the decks (the decks keep playing under it).
    auto* pRooms = new RoomsPage(&m_site, pWindow);
    m_pRooms = pRooms;
    pRooms->setSessionMenu(m_pUi->menu());
    // Your room open: to the decks (the booth, the room's strip under the
    // top bar).
    connect(pRooms, &RoomsPage::roomOpened, this, [this] { showTab(TopBar::Booth); });
    // Ask to join a room with a free side: its host lets you in.
    connect(pRooms, &RoomsPage::joinRequested, this, &Controller::askToPlay);
    // Too old to play, and the new version downloaded: Update.
    connect(pRooms, &RoomsPage::updateNowClicked, this, &Controller::installUpdate);
    // The host dialog asks how you play (starting from your last choice).
    pRooms->setPlayStyle(playStyle());
    connect(pRooms, &RoomsPage::playStyleChosen, this, &Controller::choosePlayStyle);
    // One chat, in the booth's own chat panel: your own room, or the one
    // you're playing in.
    m_pChat = new RoomChat(&m_site, pWindow);
    m_pChat->hide();
    m_site.setLinkInfo([this] {
        const QString link = m_pManager ? m_pManager->linkKind() : QString();
        return QJsonObject{{QStringLiteral("room"), link.isEmpty() ? QString() : m_pChat->roomId()},
                {QStringLiteral("link"), link}};
    });
    // The room as its chat sees it (fresh: who's in it), for the strip.
    connect(m_pChat, &RoomChat::roomChanged, this, [this](const QJsonObject& room) {
        m_chatRoom = room;
        updateChrome();
    });
    connect(m_pChat, &RoomChat::unreadChanged, this, [this](int unread) {
        m_unread = unread;
    });
    // Under the decks: the setlist, the chat, who's in the room.
    m_pPanels = new RoomPanels(m_pChat, &m_setList, pWindow);
    // As tall over the booth as last dragged, on two decks and on four
    // (kept).
    {
        const QSettings ini(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
        m_pPanels->setOverHeights(ini.value(QStringLiteral("panels_over"), 0).toInt(),
                ini.value(QStringLiteral("panels_over4"), 0).toInt());
    }
    connect(m_pPanels, &RoomPanels::heightChosen, this, [this](int px, bool four) {
        QSettings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat)
                .setValue(four ? QStringLiteral("panels_over4") : QStringLiteral("panels_over"), px);
    });
    // The host's app tells side b each track played, as it goes in.
    connect(&m_setList, &SetList::added, this, [this](const SetList::Entry& entry) {
        if (m_pManager && m_setLinked && !m_hostRoom.isEmpty()) {
            m_pManager->sendSetEntry(QJsonDocument(entry.toJson()).toJson(QJsonDocument::Compact));
        }
    });
    connect(pRooms, &RoomsPage::leavePlayingRequested, this, &Controller::leavePlaying);
    connect(pRooms, &RoomsPage::decideRequested, this,
            [this](const QString& roomId, int requestId, const QString& action) {
                m_site.decide(roomId, requestId, action, [this](const QJsonObject& room, const QString& error) {
                    if (!error.isEmpty()) {
                        QMessageBox::warning(m_pWindow, tr("Your room"), error);
                        return;
                    }
                    if (!room.isEmpty()) {
                        m_hostRoom = room;
                        updateRoomPlay();
                    }
                });
            });
    // Rooms and the B2B session: who's asking to play in our room, and the
    // room we're playing in.
    m_roomTimer.setInterval(4000);
    connect(&m_roomTimer, &QTimer::timeout, this, &Controller::pollRooms);
    m_roomTimer.start();
    connect(&m_site, &Site::roomsChanged, this, &Controller::pollRooms);
    connect(pBar, &TopBar::noticeClicked, this, [this] { showTab(TopBar::Rooms); });
    // Someone asking to play: answered from the bar, on whichever tab.
    connect(pBar, &TopBar::noticeAnswered, this, [this, pBar](bool yes) {
        const QString roomId = m_hostRoom.value(QStringLiteral("id")).toString();
        if (roomId.isEmpty() || m_noticeRequestId < 0) {
            return;
        }
        pBar->setNotice(QString()); // until the room says what's next
        m_site.decide(roomId,
                m_noticeRequestId,
                yes ? QStringLiteral("approve") : QStringLiteral("decline"),
                [this](const QJsonObject& room, const QString& error) {
                    if (!error.isEmpty()) {
                        QMessageBox::warning(m_pWindow, tr("Your room"), error);
                        return;
                    }
                    if (!room.isEmpty()) {
                        m_hostRoom = room;
                        updateRoomPlay();
                    }
                });
    });
    connect(pBar, &TopBar::tabChanged, this, &Controller::showTab);
    for (int tab : {TopBar::Rooms, TopBar::Booth}) {
        auto* pShortcut = new QShortcut(QKeySequence(Qt::CTRL | (tab == TopBar::Rooms ? Qt::Key_1 : Qt::Key_2)),
                pWindow);
        connect(pShortcut, &QShortcut::activated, this, [this, tab] { showTab(tab); });
    }
    m_chromeTimer.setInterval(250);
    connect(&m_chromeTimer, &QTimer::timeout, this, &Controller::updateChrome);
    m_chromeTimer.start();
    // The app opens on Rooms: what's going on (or, signed out, the way in).
    pBar->setSignedOut(!canUse());
    showTab(TopBar::Rooms);
    // Testing: ask to play in this room once signed in.
    if (const QString room = qEnvironmentVariable("B2B_TEST_ASK_TO_PLAY"); !room.isEmpty()) {
        QTimer::singleShot(4000, this, [this, room] { askToPlay(room, QString()); });
    }
}

// ------------------------------------------------------------ rooms & play --
// A room is where people play together: its host's app hosts the B2B
// session (once someone is let on the decks) and puts its join code in the
// room for them; a DJ the host lets in joins it with that code. No separate
// "session" for people to set up.

void Controller::pollRooms() {
    if (!m_site.signedIn()) {
        if (!m_hostRoom.isEmpty() || !m_guestRoomId.isEmpty()) {
            m_hostRoom = QJsonObject();
            m_guestRoomId.clear();
            m_guestRoom = QJsonObject();
            updateRoomPlay();
        }
        return;
    }
    if (m_pollBusy) {
        return;
    }
    // Our own open room (the site's list says which).
    const QJsonArray mine = m_site.myRooms();
    const QString hostId = mine.isEmpty() ? QString() : mine.first().toObject().value(QStringLiteral("id")).toString();
    if (hostId.isEmpty()) {
        if (!m_hostRoom.isEmpty()) {
            m_hostRoom = QJsonObject();
            updateRoomPlay();
        }
    } else {
        m_pollBusy = true;
        m_site.getRoom(hostId, [this, hostId](const QJsonObject& room, const QString& error) {
            m_pollBusy = false;
            if (error.isEmpty() && room.value(QStringLiteral("open")).toBool(true)) {
                m_hostRoom = room;
            } else if (error.isEmpty()) {
                m_hostRoom = QJsonObject();
            }
            updateRoomPlay();
        });
    }
    // The room we asked to play in (here, or on the website).
    if (m_guestRoomId.isEmpty() && hostId.isEmpty() && !m_site.askedRooms().isEmpty()) {
        m_guestRoomId = m_site.askedRooms().first().toObject().value(QStringLiteral("id")).toString();
        m_guestInvite.clear(); // asking let us in
    }
    if (!m_guestRoomId.isEmpty()) {
        m_site.getRoom(
                m_guestRoomId,
                [this, id = m_guestRoomId](const QJsonObject& room, const QString& error) {
                    if (id != m_guestRoomId) {
                        return;
                    }
                    if (!error.isEmpty() && room.isEmpty()) {
                        if (error.contains(QLatin1String("closed"), Qt::CaseInsensitive) ||
                                error.contains(QLatin1String("404"))) {
                            m_guestRoom = QJsonObject();
                            m_guestRoomId.clear();
                        }
                    } else {
                        m_guestRoom = room;
                    }
                    updateRoomPlay();
                },
                m_guestInvite);
    }
    // Not on Rooms: keep the list (and so "our room") fresh ourselves.
    if (!m_pRooms || !m_pRooms->isVisible()) {
        if (!m_sinceRoomsRefresh.isValid() || m_sinceRoomsRefresh.elapsed() > 15000) {
            m_sinceRoomsRefresh.start();
            m_site.refreshRooms();
        }
    }
}

void Controller::closeMyRoom(const QString& roomId, std::function<void()> done) {
    // Out of its session too: a room is its two DJs playing.
    if (m_sessionForRoom && m_pManager) {
        m_sessionForRoom = false;
        leave();
    }
    m_site.closeRoom(roomId, [this, done](const QJsonObject&, const QString& error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(m_pWindow, tr("Close the room"), error);
            return;
        }
        m_hostRoom = QJsonObject();
        m_site.refreshRooms();
        updateRoomPlay();
        if (done) {
            done();
        }
    });
}

void Controller::showUpdates() {
    const QString newer = m_updates.newerVersion();
    const Updater::State state = m_updater.state();
    const bool ready = state == Updater::State::Ready && m_updater.version() == newer;
    if (auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data())) {
        // While it downloads, nothing yet: it says so once it's ready.
        const bool getting = state == Updater::State::Getting;
        pBar->setUpdate(m_updates.required() || getting ? QString() : newer, ready);
    }
    if (auto* pRooms = qobject_cast<RoomsPage*>(m_pRooms.data())) {
        pRooms->setUpdateRequired(m_updates.required(), newer, m_updates.page());
        pRooms->setUpdateProgress(m_updater.canGet(), state, m_updater.percent(), m_updater.problem());
    }
}

void Controller::updateClicked() {
    auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data());
    if (!pBar || m_updater.state() != Updater::State::Ready ||
            m_updater.version() != m_updates.newerVersion()) {
        QDesktopServices::openUrl(m_updates.page());
        return;
    }
    // Never in the middle of a set: closing the app would close the room.
    QMenu menu(pBar);
    menu.setStyleSheet(theme::menu());
    if (inRoom()) {
        menu.addAction(tr("Update after your set: leave the room first"))->setEnabled(false);
    } else {
        menu.addAction(tr("Update now: pitchride closes, installs %1 and opens again")
                               .arg(m_updater.version()),
                this,
                &Controller::installUpdate);
    }
    menu.addAction(tr("Later"));
    QWidget* pNotice = pBar->updateNotice();
    menu.exec(pNotice->mapToGlobal(QPoint(0, pNotice->height())));
}

void Controller::installUpdate() {
    if (inRoom() || m_updater.state() != Updater::State::Ready || !m_pWindow) {
        return;
    }
    // Closing asks the usual questions (a deck still playing?). Said no:
    // nothing happens. Closed: the update runs as the app quits.
    m_installOnQuit = true;
    if (!m_pWindow->close()) {
        m_installOnQuit = false;
    }
}

void Controller::leaveRoomsOnQuit() {
    if (!m_site.signedIn()) {
        return;
    }
    auto pLoop = std::make_shared<QEventLoop>();
    auto pending = std::make_shared<int>(0);
    auto done = [pLoop, pending](const QJsonObject&, const QString&) {
        if (--*pending == 0) {
            pLoop->quit();
        }
    };
    if (!m_hostRoom.isEmpty()) {
        ++*pending;
        qInfo() << "B2B: closing your room as the app quits";
        m_site.closeRoom(m_hostRoom.value(QStringLiteral("id")).toString(), done);
    }
    if (!m_guestRoomId.isEmpty()) {
        ++*pending;
        qInfo() << "B2B: leaving the room you're in as the app quits";
        m_site.stopPlaying(m_guestRoomId, done);
    }
    if (*pending > 0) {
        // (Offline: the site closes it anyway once the app has been gone a while.)
        QTimer::singleShot(3000, pLoop.get(), &QEventLoop::quit);
        pLoop->exec();
    }
}

void Controller::askToPlay(const QString& roomId, const QString& invite) {
    m_site.askToPlay(roomId, invite, [this, roomId, invite](const QJsonObject& room, const QString& error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(m_pWindow, tr("Ask to join"), error);
            return;
        }
        m_guestRoomId = roomId;
        m_guestInvite = invite;
        m_guestRoom = room;
        updateRoomPlay();
    });
}

void Controller::leavePlaying() {
    if (m_guestRoomId.isEmpty()) {
        return;
    }
    m_site.stopPlaying(m_guestRoomId, [](const QJsonObject&, const QString&) {});
    m_guestRoomId.clear();
    m_guestInvite.clear();
    m_guestRoom = QJsonObject();
    if (m_sessionForRoom) {
        m_sessionForRoom = false;
        leave();
    }
    updateRoomPlay();
}

void Controller::leaveClicked() {
    // "Leave" in the bar: off the room's decks too, or the room would bring
    // the session straight back.
    if (m_sessionForRoom && !m_guestRoomId.isEmpty()) {
        leavePlaying();
        return;
    }
    if (m_sessionForRoom && !m_hostRoom.isEmpty()) {
        // The host leaving is the room ending: everyone in it is let go.
        if (!confirm(m_pWindow,
                    tr("Leave"),
                    tr("leave and close your room? your friend is let go too."),
                    tr("Leave and close"),
                    tr("Stay"))) {
            return;
        }
        closeMyRoom(m_hostRoom.value(QStringLiteral("id")).toString(), nullptr);
        return;
    }
    m_sessionForRoom = false;
    leave();
    updateRoomPlay();
}

void Controller::updateRoomPlay() {
    const auto phase = m_pUi->state().phase;
    // Hosting: once someone's on the decks, host the session and give the
    // room its code; when they've all gone, end it.
    if (!m_hostRoom.isEmpty()) {
        const bool djs = !m_hostRoom.value(QStringLiteral("on_decks")).toArray().isEmpty();
        if (djs && !m_pManager) {
            ::b2b::qt::SessionSettings settings = m_pUi->lastSettings();
            settings.host = true;
            settings.useCode = true;
            if (settings.musicFolder.isEmpty()) {
                settings.musicFolder = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
            }
            m_sessionForRoom = true;
            m_codeSent.clear();
            // Until this session's code is ready, none (not an old one).
            if (!m_hostRoom.value(QStringLiteral("session")).toObject().value(QStringLiteral("join_code")).toString().isEmpty()) {
                m_site.setSessionCode(m_hostRoom.value(QStringLiteral("id")).toString(), QString());
            }
            start(settings);
        } else if (!djs && m_sessionForRoom && m_pManager && m_guestRoomId.isEmpty()) {
            m_sessionForRoom = false;
            leave();
        }
        const QString code = m_pUi->state().code;
        const QString roomCode = m_hostRoom.value(QStringLiteral("session")).toObject()
                                         .value(QStringLiteral("join_code"))
                                         .toString();
        if (m_sessionForRoom && !code.isEmpty() && !m_pUi->state().codeIsPassword && code != roomCode &&
                code != m_codeSent) {
            m_codeSent = code;
            m_site.setSessionCode(m_hostRoom.value(QStringLiteral("id")).toString(), code, m_pUi->state().codeSecret);
        } else if (!m_sessionForRoom && !roomCode.isEmpty() && m_codeSent != QLatin1String("-")) {
            // A code from an earlier session (or app run): nobody can join
            // with it now, so take it away before a DJ tries.
            m_codeSent = QStringLiteral("-");
            m_site.setSessionCode(m_hostRoom.value(QStringLiteral("id")).toString(), QString());
        }
        // Someone new asking: say so in the bar, whichever tab we're on.
        // One at a time, answerable right there (the first asked first).
        QStringList asking;
        m_noticeRequestId = -1;
        for (const QJsonValue& value : m_hostRoom.value(QStringLiteral("requests")).toArray()) {
            const QJsonObject request = value.toObject();
            asking.append(request.value(QStringLiteral("user")).toObject().value(QStringLiteral("name")).toString());
            if (m_noticeRequestId < 0) {
                m_noticeRequestId = request.value(QStringLiteral("id")).toInt(-1);
            }
        }
        if (auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data())) {
            const QString more = asking.size() > 1 ? tr(" (+%1 more)").arg(asking.size() - 1) : QString();
            pBar->setNotice(asking.isEmpty() ? QString() : tr("%1 wants to join%2").arg(asking.first(), more),
                    m_noticeRequestId >= 0);
        }
    } else if (auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data())) {
        pBar->setNotice(QString());
    }
    // Playing in someone's room: join their session once they've said yes.
    QString guestStatus;
    if (!m_guestRoom.isEmpty()) {
        const QString status = m_guestRoom.value(QStringLiteral("my_request")).toObject()
                                       .value(QStringLiteral("status"))
                                       .toString();
        const QString code = m_guestRoom.value(QStringLiteral("session")).toObject()
                                     .value(QStringLiteral("join_code"))
                                     .toString();
        // The session secret the host's app made: it comes with the code, through the room.
        const QString secret = m_guestRoom.value(QStringLiteral("session")).toObject()
                                       .value(QStringLiteral("join_secret"))
                                       .toString();
        const bool roomOpen = m_guestRoom.value(QStringLiteral("open")).toBool(true);
        if (!roomOpen) {
            // The host closed the room: you're out, your booth as it was.
            const QString host = m_guestRoom.value(QStringLiteral("host")).toObject()
                                         .value(QStringLiteral("name")).toString();
            m_guestRoom = QJsonObject();
            m_guestRoomId.clear();
            if (m_sessionForRoom) {
                endRoomPlay(host.isEmpty() ? tr("the room has closed.") : tr("%1 closed the room.").arg(host));
            }
        } else if (status == QLatin1String("pending")) {
            guestStatus = QStringLiteral("waiting");
        } else if (status == QLatin1String("approved")) {
            // Let in: say so, wherever you are, and go to the booth.
            if (!m_guestLetIn) {
                m_guestLetIn = true;
                const QString host = m_guestRoom.value(QStringLiteral("host")).toObject()
                                             .value(QStringLiteral("name")).toString();
                showTab(TopBar::Booth);
                if (m_pWindow) {
                    QApplication::alert(m_pWindow);
                    // Once the booth is up (the note goes on what's showing).
                    const QString note = host.isEmpty() ? tr("you're in.")
                                                        : tr("%1 let you in.").arg(host.toLower());
                    QTimer::singleShot(400, this, [this, note] {
                        if (m_pWindow) {
                            toast(m_pWindow, note);
                        }
                    });
                }
            }
            const bool codeFailed = !m_failedRoomCode.isEmpty() &&
                    QString::fromStdString(::b2b::normaliseJoinCode(code.toStdString())) == m_failedRoomCode;
            if (!m_pManager && !code.isEmpty() && !secret.isEmpty() && !codeFailed) {
                m_failedRoomCode.clear();
                ::b2b::qt::SessionSettings settings = m_pUi->lastSettings();
                settings.host = false;
                settings.peerAddress = code;
                settings.password = secret;
                if (settings.musicFolder.isEmpty()) {
                    settings.musicFolder = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
                }
                m_sessionForRoom = true;
                start(settings);
            }
            guestStatus = phase == ::b2b::qt::UiState::Phase::Connected ? QStringLiteral("playing")
                                                                         : QStringLiteral("connecting");
            if (codeFailed && !m_pManager) {
                const QString host = m_guestRoom.value(QStringLiteral("host")).toObject()
                                             .value(QStringLiteral("name")).toString();
                guestStatus = tr("waiting for %1's app. they need pitchride open to play.")
                                      .arg(host.isEmpty() ? tr("the host") : host.toLower());
            }
            // Linked, then not for a while: the host left (their app closed,
            // or they left the session).
            if (m_sessionForRoom && m_pManager) {
                if (phase == ::b2b::qt::UiState::Phase::Connected) {
                    m_roomLinked = true;
                    m_sinceRoomUnlinked.invalidate();
                } else if (m_roomLinked) {
                    if (!m_sinceRoomUnlinked.isValid()) {
                        m_sinceRoomUnlinked.start();
                    } else if (m_sinceRoomUnlinked.elapsed() > 15000) {
                        const QString host = m_guestRoom.value(QStringLiteral("host")).toObject()
                                                     .value(QStringLiteral("name")).toString();
                        endRoomPlay(host.isEmpty() ? tr("the host left the decks.")
                                                   : tr("%1 left the decks.").arg(host));
                    }
                }
            }
        } else if (status == QLatin1String("cancelled") || status.isEmpty()) {
            // Taken back (by you, here or on the website), or the host took
            // you off the decks.
            m_guestRoom = QJsonObject();
            m_guestRoomId.clear();
            m_guestInvite.clear();
            if (m_sessionForRoom && m_pManager) {
                endRoomPlay(tr("you're off the decks."));
            }
        } else {
            // Declined, or taken off the decks.
            guestStatus = status == QLatin1String("denied") ? tr("the host said not this time.")
                                                            : tr("you're off the decks.");
            if (m_sessionForRoom && m_pManager) {
                endRoomPlay(status == QLatin1String("denied") ? QString() : tr("you're off the decks."));
            }
        }
    }
    if (m_guestRoom.isEmpty() || guestStatus.isEmpty() || guestStatus == QLatin1String("waiting")) {
        m_guestLetIn = false;
    }
    if (m_guestRoom.isEmpty()) {
        m_failedRoomCode.clear();
    }
    if (m_guestRoom.isEmpty() && m_guestRoomId.isEmpty() && m_sessionForRoom && m_pManager && m_hostRoom.isEmpty()) {
        endRoomPlay(tr("the room has closed.")); // gone from the site
    }
    // The room passes the join code on: no code to send anyone.
    m_pUi->setCodeHidden(m_sessionForRoom);
    updateChatRoom();
    if (auto* pRooms = qobject_cast<RoomsPage*>(m_pRooms.data())) {
        pRooms->setHostRoom(m_hostRoom);
        pRooms->setGuestRoom(m_guestRoom, guestStatus);
    }
}

void Controller::updateChatRoom() {
    if (!m_pChat) {
        return;
    }
    if (!m_hostRoom.isEmpty()) {
        m_pChat->setRoom(m_hostRoom.value(QStringLiteral("id")).toString(), QString());
    } else if (!m_guestRoomId.isEmpty()) {
        m_pChat->setRoom(m_guestRoomId, m_guestInvite);
    } else {
        m_pChat->setRoom(QString(), QString());
    }
}

void Controller::showTab(int tab) {
    // Signed out (or not invited yet, or too old to play), the app is only
    // its way in (the Rooms page's sign-in, invite code or update).
    if (!canUse()) {
        tab = TopBar::Rooms;
    }
    if (auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data())) {
        pBar->setTab(tab);
    }
    m_pointer.setCovered(tab == TopBar::Rooms); // no friend's pointer over Rooms
    if (m_pRooms) {
        const bool coming = tab == TopBar::Rooms && !m_pRooms->isVisible();
        m_pRooms->setVisible(tab == TopBar::Rooms);
        if (coming) {
            fadeIn(m_pRooms); // fades up over the booth, not a snap
        }
        if (tab == TopBar::Rooms) {
            m_pRooms->raise();
        }
    }
    updateChrome();
}

void Controller::updateChrome() {
    mapwizard::setInRoom(inRoom()); // (the wizard waits while you're in one)
    auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data());
    if (!pBar || !m_pWindow) {
        return;
    }
    // Your name on your decks' titles, once the site has it.
    if (m_pManager) {
        m_pManager->setMyName(m_site.me().value(QStringLiteral("name")).toString());
    }
    // Mixxx makes its menu bar again with a new skin (2 / 4 decks too), and
    // shows it after: keep it out of sight.
    if (QMenuBar* pMenuBar = m_pWindow->menuBar()) {
        if (pMenuBar->height() > 0) {
            pMenuBar->setNativeMenuBar(false);
            pMenuBar->setMinimumHeight(0);
            pMenuBar->setFixedHeight(0);
        }
        if (!pMenuBar->property("b2bTabKeysFreed").toBool()) {
            // Ctrl+1 / Ctrl+2 are the tabs now (Mixxx had them for skin
            // settings and the microphones).
            pMenuBar->setProperty("b2bTabKeysFreed", true);
            const QList<QKeySequence> tabKeys{QKeySequence(Qt::CTRL | Qt::Key_1), QKeySequence(Qt::CTRL | Qt::Key_2)};
            for (QAction* pAction : pMenuBar->findChildren<QAction*>()) {
                if (tabKeys.contains(pAction->shortcut())) {
                    pAction->setShortcut(QKeySequence());
                }
            }
        }
    }
    // The decks' OpenGL views (waveforms, jogs) are windows of their own,
    // drawn over any widget: out of sight while the Rooms page is up (only
    // their windows: the skin's own showing and hiding is left alone).
    const bool rooms = m_pRooms && m_pRooms->isVisible();
    if (QWidget* pCentral = m_pWindow->centralWidget()) {
        for (QWidget* pChild : pCentral->findChildren<QWidget*>()) {
            if (qstrcmp(pChild->metaObject()->className(), "QWindowContainer") != 0) {
                continue;
            }
            if (rooms && !pChild->isHidden()) {
                pChild->setProperty("b2bTabHidden", true);
                pChild->hide();
            } else if (!rooms && pChild->property("b2bTabHidden").toBool()) {
                pChild->setProperty("b2bTabHidden", QVariant());
                pChild->show();
            }
        }
    }
    // The Rooms page covers the decks (the skin can be swapped under it).
    if (rooms) {
        QWidget* pCentral = m_pWindow->centralWidget();
        const QRect area = pCentral ? pCentral->geometry() : m_pWindow->rect();
        if (m_pRooms->geometry() != area) {
            m_pRooms->setGeometry(area);
        }
        m_pRooms->raise();
    }
    // The room's chat in the booth's chat panel (made again with the skin).
    if (m_pChat && m_pPanels) {
        // (Their height dragged by their bar, two decks and four.)
        m_pPanels->setResizable(true);
        m_pPanels->attachTo(rooms ? nullptr : m_pWindow->centralWidget(), m_pWindow);
        m_pChat->setOpen(ControlObject::get(ConfigKey(QStringLiteral("[B2BBooth]"), QStringLiteral("chat_open"))) > 0);
    }
    // The room strip on the booth tab while you're in a room; the
    // session's strip in the top bar while in a session that isn't a
    // room's (the room strip says how the session is, and hands over).
    const bool stripShown = updateStrip(rooms);
    if (m_pSessionStrip) {
        m_pSessionStrip->setVisible(m_pUi->state().phase != ::b2b::qt::UiState::Phase::Idle &&
                !(m_sessionForRoom && (stripShown || rooms)));
    }
    // The room's code: in the strip on the booth tab, in the bar on Rooms.
    pBar->setRoomCode(stripShown ? QString() : m_hostRoom.value(QStringLiteral("code")).toString());
}

bool Controller::updateStrip(bool rooms) {
    auto* pStrip = qobject_cast<RoomStrip*>(m_pStrip.data());
    if (!pStrip || !m_pStripBar) {
        return false;
    }
    RoomStrip::State st;
    QJsonObject room;
    if (!m_hostRoom.isEmpty()) {
        st.role = RoomStrip::Role::Host;
        room = m_hostRoom;
    } else if (!m_guestRoomId.isEmpty()) {
        st.role = RoomStrip::Role::Guest;
        room = m_guestRoom;
    }
    // The chat's word on the room is the freshest (who's in it now).
    if (st.role != RoomStrip::Role::None && m_pChat && !m_chatRoom.isEmpty() &&
            m_chatRoom.value(QStringLiteral("id")).toString() == m_pChat->roomId()) {
        for (auto it = m_chatRoom.constBegin(); it != m_chatRoom.constEnd(); ++it) {
            room.insert(it.key(), it.value());
        }
    }
    st.room = room;
    st.style = playStyle();
    if (m_sessionForRoom && m_pManager) {
        const ::b2b::qt::UiState& ui = m_pUi->state();
        st.waiting = ui.phase == ::b2b::qt::UiState::Phase::Waiting;
        st.linked = ui.phase == ::b2b::qt::UiState::Phase::Connected;
        st.live = ui.live;
        st.deckEach = ui.deckEach;
        st.requestPending = ui.requestPending;
        st.peerAllowsTake = ui.peerAllowsTake;
        // Who plays which deck ("1 and 2"), from who owns what.
        const uint32_t mine = m_pManager->ownedDeckMask();
        auto decksText = [](uint32_t mask) {
            QStringList decks;
            for (int d = 0; d < 4; ++d) {
                if (mask & (1u << d)) {
                    decks << QString::number(d + 1);
                }
            }
            if (decks.size() < 2) {
                return decks.join(QString());
            }
            const QString last = decks.takeLast();
            return tr("%1 and %2").arg(decks.join(QStringLiteral(", ")), last);
        };
        st.myDecks = decksText(mine);
        st.friendDecks = decksText(~mine & 0xFu);
        st.friendName = m_pManager->friendName();
        st.problem = ui.problem;
    }
    // The decks' lines: your side, and who has the other.
    {
        QString mySide;
        QString other;
        if (st.role == RoomStrip::Role::Host) {
            mySide = QStringLiteral("A");
            const QJsonArray onDecks = m_hostRoom.value(QStringLiteral("on_decks")).toArray();
            if (!onDecks.isEmpty()) {
                other = onDecks.first().toObject().value(QStringLiteral("user")).toObject()
                                .value(QStringLiteral("name")).toString();
            }
        } else if (st.role == RoomStrip::Role::Guest) {
            mySide = QStringLiteral("B");
            other = room.value(QStringLiteral("host")).toObject().value(QStringLiteral("name")).toString();
        }
        if (m_pManager && !m_pManager->friendName().isEmpty()) {
            other = m_pManager->friendName();
        }
        m_nowPlaying.setSides(mySide, m_site.me().value(QStringLiteral("name")).toString(), other);
    }
    updateSetList(static_cast<int>(st.role));
    if (m_pPanels) {
        const QString host = room.value(QStringLiteral("host")).toObject().value(QStringLiteral("name")).toString().toLower();
        const QString other = (m_pManager && !m_pManager->friendName().isEmpty()
                        ? m_pManager->friendName()
                        : (st.role == RoomStrip::Role::Guest ? host : QString()))
                                      .toLower();
        QString tail;
        if (st.linked && st.deckEach) {
            tail = tr(" you play %1.").arg(st.myDecks); // ("decks 1 and 3")
        } else if (st.linked && st.live) {
            tail = tr(" hand over when your track ends.");
        } else if (st.linked) {
            tail = tr(" %1 is live.").arg(other.isEmpty() ? tr("your friend") : other);
        }
        switch (st.role) {
        case RoomStrip::Role::Host:
            m_pPanels->setRole(tr("your room"),
                    (st.linked ? tr("you're playing with %1.").arg(other.isEmpty() ? tr("your friend") : other)
                               : tr("nobody's joined yet. send a friend your room's code.")) +
                            tail);
            break;
        case RoomStrip::Role::Guest:
            m_pPanels->setRole(tr("you"),
                    st.linked ? tr("you're in %1's room.").arg(host.isEmpty() ? tr("the host") : host) + tail
                              : tr("you asked to join %1's room.").arg(host.isEmpty() ? tr("the host") : host));
            break;
        case RoomStrip::Role::None:
            m_pPanels->setRole(QString(), QString());
            break;
        }
    }
    st.panelsOpen = ControlObject::get(ConfigKey(QStringLiteral("[B2BBooth]"), QStringLiteral("chat_open"))) > 0;
    st.unread = m_unread;
    pStrip->setState(st);
    const bool shown = st.role != RoomStrip::Role::None && !rooms;
    if (m_pStripBar->isVisible() != shown) {
        m_pStripBar->setVisible(shown);
    }
    return shown;
}

void Controller::updateSetList(int role) {
    // The room it's the setlist of: another one, a new list.
    const QString roomId = m_pChat ? m_pChat->roomId() : QString();
    if (roomId != m_setRoomId) {
        m_setRoomId = roomId;
        m_setList.clear();
        m_setLinked = false;
    }
    if (roomId.isEmpty()) {
        return;
    }
    QVector<SetList::Deck> decks;
    double xf = 0;
    const auto r = static_cast<RoomStrip::Role>(role);
    if (r == RoomStrip::Role::Host || (r == RoomStrip::Role::Guest && !(m_pManager && m_pManager->peerConnected()))) {
        // Your decks (in a session, both DJs' tracks are on them). Side b
        // linked takes the host's word for it instead.
        if (r == RoomStrip::Role::Guest) {
            return; // not on the decks yet: nothing played by you here
        }
        for (int d = 0; d < 4 && m_pPlayerManager && d < static_cast<int>(m_pPlayerManager->numberOfDecks()); ++d) {
            SetList::Deck deck;
            Deck* pDeck = m_pPlayerManager->getDeck(d);
            const TrackPointer pTrack = pDeck ? pDeck->getLoadedTrack() : TrackPointer();
            if (pTrack) {
                const QString group = PlayerManager::groupForDeck(d);
                deck.id = QString::number(reinterpret_cast<quintptr>(pTrack.get())) + pTrack->getLocation();
                deck.title = pTrack->getTitle().trimmed();
                if (deck.title.isEmpty()) {
                    deck.title = QFileInfo(pTrack->getLocation()).completeBaseName();
                }
                deck.artist = pTrack->getArtist().trimmed();
                deck.play = ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0;
                deck.volume = ControlObject::get(ConfigKey(group, QStringLiteral("volume")));
                deck.orientation = static_cast<int>(ControlObject::get(ConfigKey(group, QStringLiteral("orientation"))));
                deck.side = m_nowPlaying.sideOf(d);
                deck.dj = m_nowPlaying.whoOf(d);
            }
            decks.append(deck);
        }
        xf = ControlObject::get(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")));
        // Side b has just linked: all of it so far, then each as it goes in.
        const bool linked = m_pManager && m_pManager->peerConnected();
        if (linked && !m_setLinked) {
            for (const SetList::Entry& e : m_setList.entries()) {
                m_pManager->sendSetEntry(QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact));
            }
        }
        m_setLinked = linked;
    } else {
        return;
    }
    m_setList.observe(decks, xf);
}

void Controller::leaveRoomClicked() {
    auto* pStrip = qobject_cast<RoomStrip*>(m_pStrip.data());
    const RoomStrip::Role role = pStrip ? pStrip->state().role : RoomStrip::Role::None;
    switch (role) {
    case RoomStrip::Role::Guest:
        leavePlaying();
        break;
    case RoomStrip::Role::Host:
        // Your room: leaving it is the room ending.
        if (!confirm(m_pWindow,
                    tr("Leave"),
                    tr("leave and close your room? your friend is let go too."),
                    tr("Leave and close"),
                    tr("Stay"))) {
            return;
        }
        closeMyRoom(m_hostRoom.value(QStringLiteral("id")).toString(), nullptr);
        break;
    case RoomStrip::Role::None:
        break;
    }
}

void Controller::rebuildProfileMenu() {
    auto* pBar = qobject_cast<TopBar*>(m_pTopBar.data());
    if (!pBar) {
        return;
    }
    const QString name = m_site.me().value(QStringLiteral("name")).toString();
    pBar->setProfile(m_site.signedIn() ? (name.isEmpty() ? tr("You") : name) : QString(), m_site.signingIn());
    QMenu* pMenu = pBar->profileMenu();
    pMenu->clear();
    if (!m_site.signedIn()) {
        pMenu->addAction(m_site.signingIn() ? tr("Open the sign-in page again") : tr("Sign in"),
                this,
                [this] { m_site.signIn(); });
        if (m_site.signingIn()) {
            pMenu->addAction(tr("Cancel signing in"), this, [this] { m_site.cancelSignIn(); });
        }
        return;
    }
    pMenu->addAction(name.isEmpty() ? tr("Signed in") : tr("Signed in as %1").arg(name))->setEnabled(false);
    pMenu->addSeparator();
    pMenu->addAction(tr("Your account"), this, [this] {
        QDesktopServices::openUrl(m_site.url(QStringLiteral("/me")));
    });
    // Your code to give a friend: click it to copy its invite link (the
    // friend opens it, signs in, and they're in). Used: who used it. Only
    // while the site is invite-only (otherwise it gives no codes).
    if (m_site.canPlay() && !m_friendCodes.isEmpty()) {
        pMenu->addSeparator();
        pMenu->addAction(tr("A code for a friend"))->setEnabled(false);
        for (const QJsonValue& value : std::as_const(m_friendCodes)) {
            const QJsonObject c = value.toObject();
            const QString code = c.value(QStringLiteral("code")).toString();
            if (c.value(QStringLiteral("used")).toBool()) {
                const QString who = c.value(QStringLiteral("used_by")).toString();
                pMenu->addAction(who.isEmpty() ? tr("%1  ·  used").arg(code)
                                               : tr("%1  ·  used by %2").arg(code, who))
                        ->setEnabled(false);
                continue;
            }
            pMenu->addAction(tr("%1  ·  copy the invite link").arg(code), this, [this, code] {
                const QString link = m_site.url(QStringLiteral("/invite/") + code).toString();
                QApplication::clipboard()->setText(link);
                toast(m_pWindow, tr("invite link copied. send it to a friend: it works once."));
            });
        }
    }
    if (m_site.isAdmin()) {
        pMenu->addAction(tr("Admin"), this, [this] {
            QDesktopServices::openUrl(m_site.url(QStringLiteral("/admin")));
        });
    }
    pMenu->addSeparator();
    pMenu->addAction(tr("Sign out"), this, [this] { m_site.signOut(); });
}

void Controller::refreshFriendCodes() {
    if (!m_site.canPlay()) {
        if (!m_friendCodes.isEmpty()) {
            m_friendCodes = QJsonArray();
            rebuildProfileMenu();
        }
        return;
    }
    m_site.myInvites([this](const QJsonArray& codes, const QString& error) {
        if (error.isEmpty() && codes != m_friendCodes) {
            m_friendCodes = codes;
            rebuildProfileMenu();
        }
    });
}

void Controller::openLink(const QString& link) {
    const QUrl url(link);
    qInfo() << "B2B: link" << url.scheme() + QStringLiteral("://") + url.host();
    if (m_pWindow) {
        m_pWindow->showNormal();
        m_pWindow->raise();
        m_pWindow->activateWindow();
    }
    if (url.host() == QLatin1String("room")) {
        // From a room's page on the website: the Rooms page, here.
        showTab(TopBar::Rooms);
    }
}

namespace {
// 2 decks / 4 decks: one segmented switch, square; Host: the one solid
// button (theme::bar()).
QString deckViewStyle() {
    return theme::bar() +
            QStringLiteral("QWidget#B2BDeckView { background: transparent; }"
                           "QPushButton#B2BTwoDecks { border-right: 0; }"
                           "QPushButton#B2BHostHere { margin-left: 10px; background: %1; border-color: %1; color: %2; }"
                           "QPushButton#B2BHostHere:hover { background: %3; border-color: %3; }")
                    .arg(QLatin1String(theme::kAction),
                            QLatin1String(theme::kOnAction),
                            QLatin1String(theme::kActionHover));
}
} // namespace

QWidget* Controller::makeDeckViewSwitch() {
    auto* pView = new QWidget();
    pView->setObjectName(QStringLiteral("B2BDeckView"));
    pView->setStyleSheet(deckViewStyle());
    auto* pRow = new QHBoxLayout(pView);
    pRow->setContentsMargins(0, 0, 0, 0);
    pRow->setSpacing(0);
    const bool four = LegacySkinParser::fourDecks();
    for (int decks : {2, 4}) {
        auto* pButton = new QPushButton(tr("%1 decks").arg(decks), pView);
        pButton->setObjectName(decks == 2 ? QStringLiteral("B2BTwoDecks") : QStringLiteral("B2BFourDecks"));
        pButton->setCheckable(true);
        pButton->setChecked((decks == 4) == four);
        pButton->setCursor(Qt::PointingHandCursor);
        theme::caps(pButton);
        pButton->setToolTip(decks == 2 ? tr("show two decks and the mixer")
                                       : tr("show four decks: decks 3 and 4 at the sides"));
        connect(pButton, &QPushButton::clicked, this, [this, decks] { setFourDecks(decks == 4); });
        pRow->addWidget(pButton);
    }
    // Host a room from here, without going to the Rooms tab.
    auto* pHost = new QPushButton(tr("Host"), pView);
    pHost->setObjectName(QStringLiteral("B2BHostHere"));
    pHost->setCursor(Qt::PointingHandCursor);
    theme::caps(pHost);
    pHost->setToolTip(tr("host a room: you and a friend, back to back"));
    connect(pHost, &QPushButton::clicked, this, [this] {
        auto* pRooms = qobject_cast<RoomsPage*>(m_pRooms.data());
        if (!pRooms || m_site.me().isEmpty()) {
            showTab(TopBar::Rooms); // sign in there first
            return;
        }
        pRooms->startHosting();
    });
    pRow->addWidget(pHost);
    m_pHostHere = pHost;
    m_pDeckView = pView;
    updateDeckViewSwitch();
    m_deckViewTimer.start();
    return pView;
}

void Controller::updateDeckViewSwitch() {
    if (!m_pDeckView) {
        return;
    }
    const QString skin = LegacySkinParser::lastSkinPath();
    const bool available = skin.isEmpty() || LegacySkinParser::hasFourDeckView(skin);
    const bool four = LegacySkinParser::fourDecks();
    for (auto* pButton : m_pDeckView->findChildren<QPushButton*>()) {
        if (pButton == m_pHostHere) {
            continue;
        }
        const bool isFour = pButton->objectName() == QStringLiteral("B2BFourDecks");
        pButton->setChecked(isFour == (four && available));
        pButton->setEnabled(available);
    }
    if (m_pHostHere) {
        // Not while you have a room, or are in someone's.
        m_pHostHere->setVisible(m_hostRoom.isEmpty() && m_guestRoom.isEmpty());
    }
    m_pDeckView->setToolTip(available ? QString()
                                      : tr("this skin has no 4-deck view. the b2b skins have one."));
}

void Controller::setFourDecks(bool four, bool remember) {
    if (remember) {
        QSettings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat)
                .setValue(QStringLiteral("deck_view"), four ? 4 : 2);
    }
    const bool changed = LegacySkinParser::fourDecks() != four;
    LegacySkinParser::setFourDecks(four);
    updateDeckViewSwitch();
    const QString skin = LegacySkinParser::lastSkinPath();
    if (changed && m_pWindow && (skin.isEmpty() || LegacySkinParser::hasFourDeckView(skin))) {
        qInfo() << "B2B: switching to" << (four ? 4 : 2) << "decks";
        // After this click has finished: the reload deletes the old skin.
        QMetaObject::invokeMethod(m_pWindow, "rebootMixxxView", Qt::QueuedConnection);
    }
}

void Controller::startFromEnvironment() {
    startWith(Manager::configFromEnvironment());
}

void Controller::start(const ::b2b::qt::SessionSettings& settings) {
    // The dialog decides role, address and folder; testing options can still
    // come from the environment.
    Manager::Config config = Manager::configFromEnvironment();
    config.role = settings.host ? ::b2b::Role::Leader : ::b2b::Role::Follower;
    config.musicDir = settings.musicFolder;
    // A different join-code server can be set in b2b.ini ("server=host:port").
    const QString server = QSettings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")),
            QSettings::IniFormat)
                                   .value(QStringLiteral("server"))
                                   .toString()
                                   .trimmed();
    if (!server.isEmpty() && qEnvironmentVariableIsEmpty("B2B_SERVER")) {
        config.server = server;
    }
    config.useCode = false;
    config.joinCode.clear();
    config.password = settings.password;
    // Typed or pasted as "CODE / SECRET" (the host's strip shows it so): the
    // part after the slash is the session secret.
    QString peerText = settings.peerAddress.trimmed();
    const int slash = peerText.indexOf(QLatin1Char('/'));
    if (!settings.host && slash > 0 && ::b2b::looksLikeJoinCode(peerText.left(slash).trimmed().toStdString())) {
        config.password = peerText.mid(slash + 1).trimmed();
        peerText = peerText.left(slash).trimmed();
    }
    if (settings.host) {
        config.port = settings.port;
        config.useCode = settings.useCode;
        if (config.useCode) {
            config.password.clear(); // a fresh session secret each time (b2b-core makes it)
        }
    } else if (::b2b::looksLikeJoinCode(peerText.toStdString())) {
        config.joinCode = QString::fromStdString(::b2b::normaliseJoinCode(peerText.toStdString()));
        config.peerHost.clear();
        config.port = 0;
    } else {
        const QString peer = settings.peerAddress;
        const int colon = peer.lastIndexOf(QLatin1Char(':'));
        config.peerHost = colon > 0 ? peer.left(colon) : peer;
        config.peerPort = colon > 0 ? static_cast<uint16_t>(peer.mid(colon + 1).toUInt()) : 9000;
        config.port = 0;
    }
    startWith(config);
}

void Controller::startWith(const Manager::Config& config) {
    if (m_pManager) {
        return;
    }
#ifndef B2B_DEV_SESSIONS
    // PitchRide plays through rooms: the server's join codes, never straight
    // to an address (a build with B2B_DEV_SESSIONS can, for the test tools).
    const bool direct = config.role == ::b2b::Role::Leader ? !config.useCode : config.joinCode.isEmpty();
    if (direct) {
        qWarning() << "B2B: sessions by address are off in this build; play through a room";
        return;
    }
#endif
    qInfo() << "B2B:" << (config.role == ::b2b::Role::Leader ? "hosting a session" : "joining a session");
    // The host's choice: two decks each (split) unless chosen otherwise.
    Manager::Config withDecks = config;
    withDecks.splitDecks = playStyle() == PlayStyle::Split;
    m_pManager = std::make_unique<Manager>(withDecks,
            m_pPlayerManager,
            m_pEngineMixer,
            m_pEffectsManager,
            m_settingsPath,
            m_pUi.get());
    if (!m_pManager->started()) {
        m_pManager.reset();
        m_pUi->setState({});
        QString why;
        if (config.role == ::b2b::Role::Leader) {
            why = tr("couldn't start hosting on port %1. another session or app may be using it.")
                          .arg(config.port);
        } else if (!config.joinCode.isEmpty()) {
            why = tr("couldn't reach the b2b server (%1). check your internet connection.").arg(config.server);
        } else {
            why = tr("couldn't find %1. check your friend's address.").arg(config.peerHost);
        }
        QMessageBox::warning(nullptr, tr("B2B"), why);
        return;
    }
    m_pManager->setMyName(m_site.me().value(QStringLiteral("name")).toString());
    m_owners.setManager(m_pManager.get());
    m_nowPlaying.setManager(m_pManager.get());
    m_flipTabs.setManager(m_pManager.get());
    m_shownOwnMask = 0;
    connect(m_pManager.get(), &Manager::decksChanged, this, &Controller::updateRouting);
    // The host reset the room: this copy of it too.
    connect(m_pManager.get(), &Manager::roomReset, this, [this] {
        qWarning() << "B2B: the host reset the room"; // (a warning: it reaches the log at once)
        resetRoomHere();
        if (m_pWindow) {
            const QString host = m_pManager ? m_pManager->friendName().toLower() : QString();
            toast(m_pWindow, host.isEmpty() ? tr("the host reset the room.") : tr("%1 reset the room.").arg(host));
        }
    });
    updateRouting();
    m_setLinked = false;
    // Side b: the host's app says what's been played in the room.
    connect(m_pManager.get(), &Manager::setEntryReceived, this, [this](const QByteArray& json) {
        m_setList.add(SetList::Entry::fromJson(QJsonDocument::fromJson(json).object()));
    });
    m_pointer.setManager(m_pManager.get());
    m_peerBrowser.setManager(m_pManager.get());
    connect(m_pManager.get(), &Manager::failed, this, [this, config](const QString& message) {
        leave();
        if (m_sessionForRoom && !config.joinCode.isEmpty()) {
            // A room's code that doesn't work: the host's app isn't open.
            // Said in the room, not in a box (it would come back with every
            // try); tried again when the room has a new code.
            m_failedRoomCode = config.joinCode;
            updateRoomPlay();
            return;
        }
        QMessageBox::warning(nullptr, tr("B2B"), message);
    }, Qt::QueuedConnection);
    if (m_pTrackCollectionManager) {
        m_pManager->setTrackCollectionManager(m_pTrackCollectionManager);
    }
}

void Controller::endRoomPlay(const QString& why) {
    qInfo() << "B2B: playing in the room ended:" << why;
    m_sessionForRoom = false;
    m_roomLinked = false;
    m_sinceRoomUnlinked.invalidate();
    leave();
    resetDecks();
    if (!why.isEmpty() && m_pWindow) {
        auto* pBox = new QMessageBox(QMessageBox::Information, tr("The room"),
                why + QLatin1Char('\n') + tr("your decks are yours again."), QMessageBox::Ok, m_pWindow);
        pBox->setAttribute(Qt::WA_DeleteOnClose);
        pBox->open();
    }
}

void Controller::resetDecks() {
    if (!m_pPlayerManager) {
        return;
    }
    for (unsigned int d = 0; d < m_pPlayerManager->numberOfDecks(); ++d) {
        const QString group = PlayerManager::groupForDeck(static_cast<int>(d));
        ControlObject::set(ConfigKey(group, QStringLiteral("play")), 0.0);
        // Only a deck with a track: Mixxx's eject on an empty deck brings
        // back the last track ejected (a reset would fill the empty decks).
        Deck* pDeck = m_pPlayerManager->getDeck(static_cast<int>(d));
        if (pDeck && pDeck->getLoadedTrack()) {
            pDeck->slotEjectTrack(1.0);
        }
    }
}

void Controller::leave() {
    if (!m_pManager) {
        return;
    }
    qInfo() << "B2B: leaving the session";
    m_pointer.setManager(nullptr);
    m_peerBrowser.setManager(nullptr);
    m_owners.setManager(nullptr);
    m_nowPlaying.setManager(nullptr);
    m_flipTabs.setManager(nullptr);
    m_pManager.reset();
    updateRouting(); // the controllers are yours as labelled again
    if (m_pUi) {
        m_pUi->setState({});
    }
}

void Controller::resetRoomClicked() {
    if (m_hostRoom.isEmpty() || !m_pWindow) {
        return; // (only the host resets a room)
    }
    bool playing = false;
    for (int d = 0; d < 4; ++d) {
        playing = playing ||
                ControlObject::get(ConfigKey(PlayerManager::groupForDeck(d), QStringLiteral("play"))) > 0.0;
    }
    QString text = tr("everything goes back to the start, for both of you: the tracks come out, and the "
                      "players, the mixer and the effects are reset. your master and headphones stay as they are.");
    if (playing) {
        text = tr("a deck is playing. it will stop.") + QStringLiteral("\n\n") + text;
    }
    if (!confirm(m_pWindow, tr("Reset the room"), text, tr("Reset the room"), tr("Keep it as it is"))) {
        return;
    }
    qInfo() << "B2B: resetting the room";
    if (m_pManager) {
        m_pManager->sendRoomReset(); // the friend's app resets its copy at once
    }
    resetRoomHere();
    toast(m_pWindow, tr("the room is reset."));
}

void Controller::resetRoomHere() {
    auto reset = [](const QString& group, const QString& item) {
        if (ControlObject* pControl = ControlObject::getControl(ConfigKey(group, item), ControlFlag::NoWarnIfMissing)) {
            pControl->reset();
        }
    };
    // The players: stopped and empty, their CDJ settings back.
    resetDecks();
    m_booth.resetPlayers();
    const int decks = m_pPlayerManager ? static_cast<int>(m_pPlayerManager->numberOfDecks()) : 4;
    for (int d = 0; d < decks; ++d) {
        const QString group = PlayerManager::groupForDeck(d);
        for (const char* item : {"rate", "pitch_adjust", "sync_enabled", "slip_enabled", "reverse",
                     // the channel's row on the mixer
                     "pregain", "volume", "pfl", "orientation"}) {
            reset(group, QString::fromLatin1(item));
        }
        const QString eq = QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(group);
        for (int p = 1; p <= 3; ++p) {
            reset(eq, QStringLiteral("parameter%1").arg(p));
            reset(eq, QStringLiteral("button_parameter%1").arg(p));
        }
    }
    reset(QStringLiteral("[Master]"), QStringLiteral("crossfader"));
    // The club mixer's own: COLOR, MIC and BEAT FX.
    m_mixer.resetMixer();
}

PlayStyle Controller::playStyle() const {
    if (!m_pUi->prefs().deckEach) {
        return PlayStyle::Turns;
    }
    const QSettings settings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
    return settings.value(QStringLiteral("start_decks")).toString() == QLatin1String("all") ? PlayStyle::AllMine
                                                                                          : PlayStyle::Split;
}

void Controller::choosePlayStyle(PlayStyle style) {
    if (style != PlayStyle::Turns) {
        QSettings settings(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("start_decks"),
                style == PlayStyle::AllMine ? QStringLiteral("all") : QStringLiteral("split"));
    }
    qInfo() << "B2B: how we play:" << static_cast<int>(style);
    // As if chosen in "How we play": a session switches, and tells the friend.
    m_pUi->chooseDeckEach(style != PlayStyle::Turns);
    if (m_pManager && style != PlayStyle::Turns) {
        m_pManager->setSplitDecks(style == PlayStyle::Split); // (the host's; a guest's app ignores it)
    }
    if (auto* pRooms = qobject_cast<RoomsPage*>(m_pRooms.data())) {
        pRooms->setPlayStyle(style);
    }
    updateChrome();
}

void Controller::updateRouting() {
    // Channel n of a controller plays decks[n]: first the decks that are
    // yours, in order, then the friend's. The host on decks 1 and 2 (or
    // all four) is as labelled; the friend on 3 and 4 gets them on 1 and 2.
    std::array<int, 4> decks{0, 1, 2, 3};
    bool ignoreCrossfader = false;
    if (m_pManager && m_pManager->playingDeckEach()) {
        const uint32_t mine = m_pManager->ownedDeckMask();
        int n = 0;
        for (int d = 0; d < 4; ++d) {
            if (mine & (1u << d)) {
                decks[n++] = d;
            }
        }
        for (int d = 0; d < 4; ++d) {
            if (!(mine & (1u << d))) {
                decks[n++] = d;
            }
        }
        // Two people, two controllers, one mix: their crossfaders would
        // fight. The channel faders mix; the crossfader on screen still works.
        ignoreCrossfader = m_pManager->friendJoined();
        // Two decks on screen: when the decks you own change (the split
        // starting, a deck handed over), a player showing someone else's
        // deck turns to the one of yours behind it. Yours stay put.
        if (mine != m_shownOwnMask) {
            m_shownOwnMask = mine;
            for (int side = 0; side < 2; ++side) {
                const int shown = m_booth.shownDeck(side);
                const int behind = shown ^ 2;
                if (!(mine & (1u << shown)) && (mine & (1u << behind))) {
                    m_booth.showDeck(behind);
                }
            }
        }
    }
    routing::set(decks, ignoreCrossfader);
}

} // namespace mixxx::b2b

#include "moc_b2bcontroller.cpp"
