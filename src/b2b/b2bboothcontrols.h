#pragma once

#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>

class ControlObject;
class ControlProxy;
class ControlPushButton;

namespace mixxx::b2b {

/// Controls the B2B skins use for the deck screens. Each player's
/// screen shows either the deck (waveform, time, BPM) or the library browser;
/// the library can only be in one place, so opening it on one deck returns the
/// other deck's screen to its deck view.
///
///   [B2BBooth],deckN_show_view / deckN_show_browse   buttons (press)
///   [B2BBooth],deckN_page      0 = deck view, 1 = browse (the screen's stack)
///   [B2BBooth],deckN_viewing / deckN_browsing        1 while shown (for LEDs)
///
/// Four decks on two players: the left player shows deck 1 or 3, the right
/// one deck 2 or 4 (DECK buttons above the screen).
///
///   [B2BBooth],deckN_select      button (press): show deck N on its player
///   [B2BBooth],deckN_selected    1 while deck N is the one shown
///   [B2BBooth],left_page / right_page   0 = deck 1 / 2, 1 = deck 3 / 4
///
/// The mixer's crossfader curve switch, like a hardware mixer's:
///   [B2BBooth],xf_curve   0 = smooth, 1 = mix, 2 = cut (sets Mixxx's
///                         [Mixer Profile] curve and mode)
///
/// Tempo range button on each player, as a CDJ's (b2btemporange.h): each
/// press the next of ±6, ±10, ±16, WIDE (±100 %), every deck starting on
/// ±6. The fader stays where it is, so the speed changes with the range.
///   [B2BBooth],deckN_range         button: each press, the next range
///   [B2BBooth],deckN_tempo_range   0..3, the range now ([ChannelN],rateRange;
///                                  a friend's deck's too, in a session)
///
/// The chat column beside the booth (remembered between runs):
///   [B2BBooth],chat_open   1 = shown, 0 = hidden (the tab next to it toggles)
/// Four decks: the room's panels under the booth, a slim strip or open:
///   [B2BBooth],panels_open   1 = open, 0 = the slim strip (its header toggles)
///
/// Beat grid buttons shown on a deck's screen instead of its time row
/// (skins that have no room for them otherwise):
///   [B2BBooth],deckN_grid   1 while shown (toggle)
///
/// CDJ screen keys that open the browser at a list:
///   [B2BBooth],deckN_tag_list   the Auto DJ queue (Mixxx's tag list)
///   [B2BBooth],deckN_playlist   Playlists
///
/// Double-click a deck's BPM readout (object name "ScreenBpmN" or "BpmN") to type a BPM.
///
/// A deck with no track shows a standby page on its screen instead (the
/// waveform and readouts hide), and powers up when a track loads:
///   [B2BBooth],deckN_standby    1 while the standby page is shown
///   [B2BBooth],deckN_powering   set by the screen while it powers up
///
/// Other skins never touch these.
class BoothControls : public QObject {
    Q_OBJECT
  public:
    explicit BoothControls(int decks = 4, QObject* pParent = nullptr);
    /// A player turning round to another deck (side 0 left, 1 right): the
    /// hook does the switch with apply(), when it likes (an animation). None:
    /// at once.
    void setFlipHook(std::function<void(int side, std::function<void()> apply)> hook) {
        m_flipHook = std::move(hook);
    }
    ~BoothControls() override;

    /// Two decks on screen: turn that side's player to this deck (0..3;
    /// deck d is on side d % 2), with the flip.
    void showDeck(int deck) {
        selectDeck(deck);
    }
    /// The deck a side (0 left, 1 right) shows now.
    int shownDeck(int side) const;
    /// Reset the room: every player's TEMPO range back to the first step,
    /// and its screen back from the browser to the deck.
    void resetPlayers();

  private:
    struct Deck {
        std::unique_ptr<ControlPushButton> showView;
        std::unique_ptr<ControlPushButton> showBrowse;
        // The CDJ's keys round the browse knob, and its USB slots.
        std::unique_ptr<ControlPushButton> back;
        std::unique_ptr<ControlPushButton> listUp;
        std::unique_ptr<ControlPushButton> listDown;
        std::unique_ptr<ControlPushButton> usb;
        // The friend's browser is on this player's screen (PeerBrowser).
        std::unique_ptr<ControlObject> peerBrowsing;
        std::unique_ptr<ControlObject> page;
        std::unique_ptr<ControlObject> viewing;
        std::unique_ptr<ControlObject> browsing;
        std::unique_ptr<ControlPushButton> select;
        std::unique_ptr<ControlObject> selected;
        std::unique_ptr<ControlPushButton> range;
        std::unique_ptr<ControlObject> rangeIndex; // 0..3: ±6, ±10, ±16, WIDE
        std::unique_ptr<ControlProxy> rateRange;   // Mixxx's, once the deck exists
        std::unique_ptr<ControlPushButton> grid;
        std::unique_ptr<ControlPushButton> tagList;
        std::unique_ptr<ControlPushButton> playlist;
        // No track yet: the screen shows its standby page, not the deck.
        std::unique_ptr<ControlObject> standby;
        std::unique_ptr<ControlObject> powering; // the power-up animation is playing
        std::unique_ptr<ControlProxy> trackLoaded;
        double lastLoaded = 0;
    };
    /// Open the browser on this deck's screen at a sidebar item.
    void browseTo(int deck, const QString& sidebarItem);
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;
    void askForBpm(int deck, QWidget* pNear);
    void selectDeck(int deck);
    void sideChanged(int side);
    void setPage(int deck, int page);
    void pageChanged(int deck);
    void updateScreen(int deck);
    void watchTracks();
    /// Range i on deck n (1-based), the fader where it is.
    void applyRange(int n, int i);
    /// Every deck on the first range, once the decks exist.
    void startRanges();
    /// The deck screen is narrow: show only Title, Artist, BPM, Key and
    /// Duration in the browser's track list (checked while browsing, because
    /// every sidebar item brings its own column set).
    void fitColumns();

    std::vector<Deck> m_decks;
    std::unique_ptr<ControlObject> m_sidePage[2]; // left (decks 1, 3), right (2, 4)
    std::unique_ptr<ControlProxy> m_focusedWidget;
    std::unique_ptr<ControlPushButton> m_xfCurve;
    std::unique_ptr<ControlPushButton> m_chatOpen;
    std::unique_ptr<ControlPushButton> m_panelsOpen;
    void applyXfCurve(int position);
    QTimer m_columnTimer;
    QTimer m_trackTimer; // looks for decks' track_loaded until all are there

    std::function<void(int, std::function<void()>)> m_flipHook;
};

} // namespace mixxx::b2b
