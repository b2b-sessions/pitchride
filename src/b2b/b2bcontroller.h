#pragma once

#include <QJsonArray>

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "b2b/b2bboothcontrols.h"
#include "b2b/b2bcdjcontrols.h"
#include "b2b/b2bdeckowners.h"
#include "b2b/b2bnowplaying.h"
#include "b2b/b2baboutgear.h"
#include "b2b/b2bloopreadout.h"
#include "b2b/b2bsetlist.h"
#include "b2b/b2bdeckscreens.h"
#include "b2b/b2bexplorer.h"
#include "b2b/b2bfliptabs.h"
#include "b2b/b2bmappingwizardui.h"
#include "b2b/b2blinks.h"
#include "b2b/b2bmixercontrols.h"
#include "b2b/b2bplaystyle.h"
#include "b2b/b2bmanager.h"
#include "b2b/b2bpeerbrowser.h"
#include "b2b/b2bpointer.h"
#include "b2b/b2bsite.h"
#include "b2b/b2bupdater.h"
#include "b2b/b2bupdates.h"
#include "b2b/qt/session_ui.hpp"

#include "preferences/usersettings.h"

class ControlProxy;
class QAction;
class EffectsManager;
class EngineMixer;
class Library;
class PlayerManager;
class QMainWindow;
class TrackCollectionManager;

class ControlPushButton;

class ControllerManager;

namespace mixxx::b2b {


/// Lives as long as Mixxx: owns the B2B menu and status strip (from b2b-core's
/// Qt module) and starts or ends a session (a Manager) when the user hosts,
/// joins or leaves. With B2B_ROLE set, a session starts straight away.
class Controller : public QObject {
    Q_OBJECT
  public:
    Controller(PlayerManager* pPlayerManager,
            EngineMixer* pEngineMixer,
            std::shared_ptr<EffectsManager> pEffectsManager,
            const QString& settingsPath,
            QObject* pParent = nullptr);
    ~Controller() override;

    /// Once the library exists (borrowed tracks are removed from it again).
    void setTrackCollectionManager(std::shared_ptr<TrackCollectionManager> pTrackCollectionManager);

    /// Puts the B2B menu into the window's menu bar and the status strip
    /// under it. Call again whenever the window recreates its menu bar.
    void installInto(QMainWindow* pWindow);
    /// The music library (the players' music browser adds folders to it and
    /// has tracks analysed).
    void setLibrary(Library* pLibrary);
    /// Mixxx's settings: the key notation ([B2B],key_numbers flips it).
    void setConfig(UserSettingsPointer pConfig);
    /// The controllers (the mapping wizard teaches them on the booth).
    void setControllerManager(std::shared_ptr<ControllerManager> pManager);

  private:
    void start(const ::b2b::qt::SessionSettings& settings);
    void startFromEnvironment();
    void startWith(const Manager::Config& config);
    void leave();
    /// Playing in someone's room: it ended (closed, the host left, taken off
    /// the decks). Out of the session, your decks stopped and emptied, and why.
    void endRoomPlay(const QString& why);
    /// Your decks stopped and emptied (the room's tracks go with them).
    void resetDecks();
    void sendReport(const ::b2b::qt::ReportRequest& request);
    void sendReportJson(QJsonObject report);
    // The last run crashed: offer its crash report.
    void checkForCrashReports();
    void openCrashReports();
    QString reportServer() const;
    // The 2 DECKS / 4 DECKS switch at the left of the session strip.
    /// remember: your choice (kept), not a room's.
    void setFourDecks(bool four, bool remember = true);
    void updateDeckViewSwitch();
    // A pitchride:// link (from the website).
    void openLink(const QString& link);
    // PitchRide's own window chrome: the top bar, and the Rooms page over
    // the decks.
    void installChrome(QMainWindow* pWindow);
    void showTab(int tab);
    void updateChrome();
    void rebuildProfileMenu();
    /// Your codes to give a friend, from the site (shown in the profile menu).
    void refreshFriendCodes();
    QJsonArray m_friendCodes;
    void pollRooms();
    void askToPlay(const QString& roomId, const QString& invite);
    /// Your room closed (its session ended); then done.
    void closeMyRoom(const QString& roomId, std::function<void()> done);
    void leavePlaying();
    /// The app is quitting: your open room closed, or your side of
    /// someone's room given up (waits a moment for the site).
    void leaveRoomsOnQuit();
    void updateChatRoom();
    void updateRoomPlay();
    void leaveClicked();
    /// The room strip's Leave: out of the room you play in, or your own
    /// room closed.
    void leaveRoomClicked();
    /// The room strip as things are; true: on show.
    bool updateStrip(bool rooms);
    /// The setlist, from the decks you see (a few times a second).
    void updateSetList(int role);
    QWidget* makeDeckViewSwitch();

    PlayerManager* m_pPlayerManager;
    EngineMixer* m_pEngineMixer;
    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    QString m_settingsPath;
    std::unique_ptr<::b2b::qt::SessionUi> m_pUi;
    std::unique_ptr<Manager> m_pManager;
    QPointer<QWidget> m_pToolBar;
    std::thread m_reportThread;
    BoothControls m_booth; // deck screen pages for the B2B skins
    CdjControls m_cdj;     // CDJ behaviour for the B2B skins' players
    MixerControls m_mixer; // the B2B skins' mixers
    LibraryExplorer m_explorer;  // the music browser on the players' screens
    PeerBrowser m_peerBrowser;   // and the friend's, to look at
    // Keys as numbers (8A) or letters (Am): click a key on a screen.
    void applyKeyNumbers(bool numbers);
    UserSettingsPointer m_pConfig;
    std::unique_ptr<ControlPushButton> m_pKeyNumbers;
    DeckScreensUi m_deckScreens; // standby / power-up on the players' screens
    PointerShare m_pointer; // in a session: each DJ sees the other's pointer
    DeckOwnersUi m_owners; // "a deck each": who plays each deck, + to ask
    FlipTabsUi m_flipTabs{&m_booth}; // two decks on screen: the deck behind each player
    MappingWizardUi m_mappingWizard;  // Settings > Mapping wizard, on the booth
    uint32_t m_shownOwnMask = 0;     // the decks we owned when the players last turned to them
    NowPlayingUi m_nowPlaying; // the copper line above each deck
    AboutGearUi m_aboutGear;   // the "i" on each deck and the mixer
    LoopReadoutUi m_loopReadout; // the loop's length on each deck's screen
    QPointer<QMainWindow> m_pWindow;
    QPointer<QWidget> m_pDeckView;
    QPointer<class DeckFlip> m_pDeckFlip;
    bool m_menuReady = false; // the B2B menu's items added (once)
    QPointer<class DialogStyler> m_pDialogStyler;
    QPointer<QPushButton> m_pHostHere; // HOST, next to 2 / 4 DECKS
    QTimer m_deckViewTimer; // the skin can change: is there a 4-deck view?
    Site m_site;            // the PitchRide website: signing in, rooms
    UpdateCheck m_updates;  // a newer PitchRide (or this one too old to play)
    Updater m_updater{&m_site, &m_updates}; // gets it and installs it (Windows)
    bool m_installOnQuit = false;           // Update said yes: install as the app quits
    /// The bar's notice and the too-old page, as the update stands.
    void showUpdates();
    /// The bar's notice: ready, a menu (Update now / Later); else the website.
    void updateClicked();
    /// Close the app and install the update (never while in a room).
    void installUpdate();
    /// How you play in a room (your last choice as host; split unless you
    /// chose otherwise).
    PlayStyle playStyle() const;
    /// The host chose (the host dialog, the strip's Change): remembered,
    /// and the session switches now if there is one.
    void choosePlayStyle(PlayStyle style);
    /// The host's Reset (room strip): asks first (saying so if decks are
    /// playing), then resets the room here and in the friend's app.
    void resetRoomClicked();
    /// The room back to the start, in this app: tracks out and the players
    /// stopped (tempo, range, sync, slip, key), the mixer (trims, EQs and
    /// kills, colour, faders, cue buttons, crossfader) and its effects.
    /// Master and headphones stay (each person's own), as does who plays
    /// which deck.
    void resetRoomHere();
    /// Controllers in a room: your controller's channels 1 and 2 are your
    /// own decks, and while you play a deck each, controllers' crossfaders
    /// move nothing (b2bcontrollerrouting).
    void updateRouting();
    /// Hosting a room, or playing in someone's.
    bool inRoom() const {
        return !m_hostRoom.isEmpty() || !m_guestRoomId.isEmpty();
    }
    /// Signed in, invited, and not too old: the app may be used.
    bool canUse() const {
        return m_site.canPlay() && !m_updates.required();
    }
    QPointer<QWidget> m_pTopBar;
    QPointer<QWidget> m_pRooms;
    class RoomChat* m_pChat = nullptr; // the chat, in the booth's panels
    class RoomPanels* m_pPanels = nullptr; // under the decks: setlist, chat, in the room
    SetList m_setList;                  // the room's setlist
    QString m_setRoomId;                // the room it's the setlist of
    bool m_setLinked = false;           // side b linked: it has had the setlist so far
    QPointer<QWidget> m_pSessionStrip;
    QPointer<QWidget> m_pStripBar; // the room strip's row, under the top bar
    QPointer<QWidget> m_pStrip;    // RoomStrip
    QJsonObject m_chatRoom;        // the room, as its chat last described it
    int m_unread = 0;              // chat messages not seen (panels closed)
    QTimer m_chromeTimer;   // keeps the Rooms page over the decks, the bar up to date
    QTimer m_roomTimer;
    QElapsedTimer m_sinceRoomsRefresh;
    bool m_pollBusy = false;
    QJsonObject m_hostRoom;   // our open room, as its host sees it
    int m_noticeRequestId = -1; // the request the top bar's notice answers
    bool m_roomLinked = false;          // playing in someone's room: linked at least once
    QElapsedTimer m_sinceRoomUnlinked;   // ... and since when not
    QString m_guestRoomId;    // the room we asked to play in
    QString m_guestInvite;
    QJsonObject m_guestRoom;
    bool m_guestLetIn = false; // the host said yes (you were told, and taken to the booth)
    bool m_sessionForRoom = false; // the B2B session is the room's
    QString m_codeSent;
    /// Guest: the room's join code that didn't work (the host's app isn't
    /// running it): not tried again until the room has another one.
    QString m_failedRoomCode;
    LinkServer m_links;     // pitchride:// links from later copies of the app
};

} // namespace mixxx::b2b
