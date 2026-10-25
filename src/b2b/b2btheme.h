#pragma once

#include <QFont>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace mixxx::b2b {

/// PitchRide's look around the decks: one place for its colours, type and
/// the kinds of button, so the bar, the Rooms page, the room's bar and every
/// dialog look like one app. DESIGN.md (sections 2, 3 and 8) is the spec.
///
///   Primary   solid action orange: the one thing to do (Host a room, Go in)
///   Secondary a 1 px outline in the type colour: other actions
///   Quiet     text only: Cancel
///   Danger    the keyline red outline: something to undo (Close room)
/// All flat, square, DM Mono uppercase. No gradients, no glow.
namespace theme {

// The blue world (DESIGN.md 2): the page is one continuous gradient
// through these (paintPage), never two flat colours meeting.
inline constexpr const char* kGround = "#112283";
inline constexpr const char* kGroundLit = "#1B35A6";
inline constexpr const char* kDeep = "#0A1555";
inline constexpr const char* kBoothDeep = "#05081F";
// The one red line: thin rules and frames, never fills.
inline constexpr const char* kKeyline = "#E8483C";
// Type.
inline constexpr const char* kType = "#F7D6DC";
inline constexpr const char* kTypeMuted = "#AAB7F2";
// The one action colour: primary buttons, the active tab, the maze's route.
inline constexpr const char* kAction = "#F2511E";
inline constexpr const char* kActionHover = "#F46A3D";
inline constexpr const char* kOnAction = "#FFF6F2";
// The maze's walls, deck labels, now-playing lines, chat names.
inline constexpr const char* kCopper = "#E3A06B";
// Dialogs only, when a paper panel is needed.
inline constexpr const char* kPaper = "#E8E1D0";
// The decks (DESIGN.md 7).
inline constexpr const char* kGunmetal = "#15171A";
// On air, in the LCD readouts (the action orange, lit).
inline constexpr const char* kLcd = "#FF8A5C";

// The roles the rest of the app asks for, in these colours.
inline constexpr const char* kBar = kGround;
inline constexpr const char* kPage = kGround;
inline constexpr const char* kCard = kDeep;            // a field, a menu
inline constexpr const char* kRaised = "#16289A";      // pressed, selected
inline constexpr const char* kLine = "#2A3A9E";        // a faint divider on the blue
inline constexpr const char* kLineStrong = "#4A5BB8";  // a field's outline
inline constexpr const char* kText = kType;
inline constexpr const char* kMuted = kTypeMuted;
inline constexpr const char* kFaint = "#7584C9";
inline constexpr const char* kAccent = kAction;
inline constexpr const char* kAccentHover = kActionHover;
inline constexpr const char* kOnAccent = kOnAction;
inline constexpr const char* kLive = kLcd;
inline constexpr const char* kDanger = kKeyline;

/// The four faces (DESIGN.md 3), bundled in res/fonts.
enum class Face {
    Display, // Hanken Grotesk 800: room names, page titles, the wordmark
    Label,   // DM Mono: uppercase micro labels, codes, times, counts
    Voice,   // Courier Prime: progress reports, typed notes
    Readout, // DotGothic16: the LCD bits (live counters)
    Body,    // Hanken Grotesk 400: running text in the booth's panels (the setlist, chat)
};
/// A face at a pixel size. (Each named by its own family, so it is found
/// the same way on Linux, Windows and macOS.)
QFont font(Face face, int pixelSize);
/// The face for a style sheet: `font-family: ...;` (and its weight).
QString css(Face face);

/// The drawn line icons (never a font's glyph or an emoji), on a 16 grid.
enum class Icon {
    Record,     // a record with its arm: who's on the decks
    Flip,       // two arrows chasing round: turn a player to its other deck
};
/// `which` drawn `size` px square in `colour`, sharp on a screen of `dpr`.
QPixmap icon(Icon which, int size, const QColor& colour, qreal dpr = 3.0);

/// The buttons, by object name (Primary / Secondary / Quiet / Danger),
/// sized for a page (`compact`: for a bar).
QString buttons(bool compact = false);
/// A dialog's whole look (labels, fields, buttons).
QString dialog();
/// What sits in the top bar (its labels, compact buttons, a slider):
/// on the page's blue, as the bar is.
QString bar();
/// A drop-down menu: deep blue in the keyline, square, DM Mono.
QString menu();

/// A button's label in capitals (a style sheet can't: it keeps the
/// widget's own capitalization, and sets the rest of the font).
void caps(QWidget* pWidget);

/// The page's one gradient (DESIGN.md 2), painted over `rect` of `pWidget`.
/// It is laid out over the whole window (lit at the top left, deep at the
/// bottom), so the bar and the page under it, each painting its own part,
/// meet with no edge.
void paintPage(QPainter& p, const QWidget* pWidget, const QRect& rect);

} // namespace theme

/// A short note at the bottom of the window ("Copied", "Asked for deck 3")
/// that fades in, waits and fades out by itself.
void toast(QWidget* pWindow, const QString& text);

/// `pWidget` fades in (a page coming up), quickly.
void fadeIn(QWidget* pWidget, int ms = 160);

/// Every message box and small dialog the app shows, Mixxx's own too:
/// dark, like the rest of PitchRide, not a grey system box with coloured
/// balls on its buttons. Installed on the application.
class DialogStyler : public QObject {
    Q_OBJECT
  public:
    explicit DialogStyler(QObject* pParent = nullptr);

  protected:
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    void style(QWidget* pDialog);
};

} // namespace mixxx::b2b
