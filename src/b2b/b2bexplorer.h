#pragma once

#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <functional>

class ControlProxy;
class Library;
class QAbstractItemModel;
class QLabel;
class QListWidget;
class QStackedWidget;
class QToolButton;
class QWidget;
class SidebarModel;
class TrackModel;
class WLibrary;
class WLibrarySidebar;
class WSearchLineEdit;
class WTrackTableView;

namespace mixxx::b2b {

class ColumnBar;

/// The music browser on the players' screens, laid out like a CDJ's:
///
/// - SOURCE: where the music is (rekordbox, a USB drive, this computer), as
///   three big cards. Once one connects: BROWSE.
/// - BROWSE: the source's categories (Playlist, Artist, Album, Track, Genre,
///   Key, BPM, Search, Tag List) with counts, and a slim icon bar of
///   shortcuts down the left.
/// - lists (playlists, artists...) one level at a time, and
/// - the TRACK LIST: the list's name, where you are, and the tracks as a
///   CDJ shows them (number, artwork, artist - title, date added, BPM, key).
///
/// It is drawn by the app in the skin's B2BExplorer slot, over Mixxx's own
/// library: the library's features (rekordbox, the computer's folders, your
/// Mixxx library) are still what lists and loads the music, and its track
/// table shows the tracks, restyled. The library's sidebar stays in the skin,
/// hidden.
class LibraryExplorer : public QObject {
    Q_OBJECT
  public:
    explicit LibraryExplorer(QObject* pParent = nullptr);
    ~LibraryExplorer() override;
    void setWindow(QWidget* pWindow);
    void setLibrary(Library* pLibrary);
    void setSettingsPath(const QString& path);

    /// The browse knob, while a list is on screen (true: used).
    static bool moveSelection(int delta);
    static bool activateSelection();
    /// The CDJ's BACK key: one step back (false: nothing to go back to).
    static bool goBack();
    /// A USB slot: SOURCE, then the USB drive.
    static void openUsb();

    /// What the browser shows, for the friend to see (PeerBrowser).
    struct Snapshot {
        int deck = 0;          // the player it's on (1-4)
        QString heading;
        QString crumb;
        bool tracks = false;   // rows: "001\t124.0\t8A\tArtist - Title"; else "label\tdetail"
        QStringList rows;      // what's on screen, at most 10
        int current = -1;      // the cursor, in rows
        int total = 0;         // tracks in the list
    };
    /// false: the browser isn't open on any player.
    static bool snapshot(Snapshot* pOut);

  private:
    enum class Page { Source, Browse, List, Tracks };
    enum class SourceKind { None, Rekordbox, Usb, Computer, Folders };
    struct Entry {
        QString label;
        QString detail;  // shown small, after the label (e.g. a count)
        int icon = 0;    // see Icon in the .cpp
        bool folder = true;
        bool accent = false;
        std::function<void()> open;
        int playlistId = -1; // one of your playlists: right-click to rename / delete
    };

    // Building and finding things.
    void refresh();
    void build(QWidget* pHost);
    void buildLoadBars();
    QModelIndex topNode(const QString& name) const;
    QString title(const QModelIndex& index) const;
    WTrackTableView* trackView() const;
    TrackModel* trackModel() const;

    // Pages.
    void showSource();
    void showBrowse();
    void showList(const QString& heading, const QString& crumb, const QList<Entry>& entries);
    void showTree(const QModelIndex& parent, const QString& crumb, const QString& name = QString());
    void showTracks(const QModelIndex& node, const QString& heading, const QString& crumb,
            const QString& filter = QString());
    void showValues(int category);
    // Your playlists (the Mixxx library's).
    void newPlaylist(const std::function<void()>& refresh);
    void playlistMenu(const Entry& entry, const QPoint& globalPos);
    /// Right-click on tracks: add to a playlist (or a new one), the tag list.
    void trackMenu(const QPoint& globalPos);
    /// A new playlist of a folder's music, named after it.
    void newPlaylistFromFolder(const std::function<void()>& refresh);
    /// Music files dropped (or a folder's) into a playlist.
    void addToPlaylist(int playlistId, const QStringList& files);
    int currentPlaylistId() const; // the track list on screen, if it's a playlist
    int createUniquePlaylist(const QString& name); // "name", "name 2"...
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;
    void acceptDrops(QWidget* pWidget);
    void setPage(Page page);
    void setHead(const QString& heading, const QString& crumb, bool back, bool upper = true);
    void pushBack(std::function<void()> fn);
    void back();

    // Sources.
    void connectRekordbox(bool usb, bool interactive);
    void connectComputer();
    void connectFolders();
    void waitForRekordbox(bool usb, bool interactive, int triesLeft, bool mayAsk = true);
    void openSource();
    QString sourceName() const;
    void saveSource();

    // The track list.
    void tick();
    void configureTable();
    void computeCounts();
    void queueAnalysis();
    void applyTextSize();
    void cycleTextSize();
    int rowHeight() const;
    int textPx() const;

    QPointer<QWidget> m_pWindow;
    QPointer<QWidget> m_pHost;
    QPointer<Library> m_pLibraryObject;
    QPointer<WLibrarySidebar> m_pSidebar;
    QPointer<WLibrary> m_pLibrary;
    QPointer<SidebarModel> m_pModel;
    QPointer<WSearchLineEdit> m_pSearch;
    QPointer<QStackedWidget> m_pStack;
    QPointer<QWidget> m_pHead;
    QPointer<QLabel> m_pHeading;
    QPointer<QLabel> m_pCrumb;
    QPointer<QToolButton> m_pBack;
    QPointer<QToolButton> m_pTextSize;
    QPointer<QListWidget> m_pCategories;
    QPointer<QListWidget> m_pList;
    QPointer<QWidget> m_pSourcePage;
    QPointer<QWidget> m_pEmpty;
    QPointer<QWidget> m_pEmptyCard;
    QPointer<ColumnBar> m_pColumns;
    QList<QWidget*> m_cards;
    QList<Entry> m_browseEntries;
    QList<Entry> m_listEntries;
    QList<std::function<void()>> m_backStack;
    std::function<void()> m_refreshList; // shows the current list again
    Page m_page = Page::Source;
    SourceKind m_source = SourceKind::None;
    QPersistentModelIndex m_libraryNode; // the source's "all tracks"
    QString m_settingsPath;
    QString m_trackHeading;
    QString m_trackCrumb;
    QString m_pendingFilter;
    int m_textSize = 1; // 0 small, 1 medium, 2 large
    QPointer<QAbstractItemModel> m_configuredModel;
    QPointer<QWidget> m_configuredView;
    QSet<qint64> m_analysisQueued;
    struct Counts {
        int tracks = -1;
        int artists = -1;
        int albums = -1;
        int genres = -1;
        int keys = -1;
        int bpms = -1;
        int playlists = -1;
    } m_counts;
    QTimer m_timer;
    QTimer m_tick;
    QPointer<ControlProxy> m_pMoveUp;
    QPointer<ControlProxy> m_pMoveDown;
    bool m_opening = false;
};

} // namespace mixxx::b2b
