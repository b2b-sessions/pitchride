#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <memory>

#include "controllers/midi/midimessage.h"
#include "preferences/configobject.h"
#include "preferences/usersettings.h"

#include "controllers/controller.h" // (a signal carries one: the whole class)

class ControllerManager;
class LegacyMidiControllerMapping;

namespace mixxx::b2b::mapwizard {

/// The mapping wizard (Settings > Mapping wizard, then the booth's strip):
/// a DJ whose controller doesn't act right clicks something on PitchRide's
/// decks or mixer and moves it on the controller, and the two are bound.
///
/// What's learned goes on top of the mapping in use, saved as a copy in the
/// user's mappings folder: "Pioneer DDJ-200 (yours)". The bundled mapping
/// is never changed; Back to the original switches to it again. Jog wheels
/// and lights stay the mapping's own.
///
/// This part has no screens: the names, the files, the changes.

/// A control in PitchRide's words ("DECK 1 · PLAY", "MIXER · CH 2 HI",
/// "BEAT FX · ON"); "" for anything not on the decks or mixer (those
/// can't be learned).
QString controlName(const ConfigKey& key);

/// The controllers the wizard can teach: MIDI ones (the only kind Mixxx
/// learns), as the controller manager found them.
QList<::Controller*> midiControllers(ControllerManager* pManager);

/// The mapping files of a controller.
struct Files {
    QString current;     ///< in use ("" none)
    QString currentName; ///< its name ("Pioneer DDJ-200 (yours)")
    QString original;    ///< what "(yours)" was made from ("" none)
    bool yours = false;  ///< the one in use is the wizard's copy
};
Files files(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController);

/// What the wizard changed: in "(yours)", each control bound by MIDI
/// inputs the original doesn't have (a button's press and release: one).
struct Change {
    QList<uint16_t> midiKeys;
    ConfigKey control;
    QString name; ///< controlName(control)
};
QList<Change> changes(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController);

/// Save what was learned on top of the mapping in use, as "(yours)" (the
/// same file again if it already is), and switch the controller to it.
/// pName: the name saved as. False: it couldn't be saved.
bool save(UserSettingsPointer pConfig,
        ControllerManager* pManager,
        ::Controller* pController,
        const MidiInputMappings& learned,
        QString* pName = nullptr);
/// Undo one change in "(yours)": those MIDI inputs as the original has them.
bool removeChange(UserSettingsPointer pConfig,
        ControllerManager* pManager,
        ::Controller* pController,
        const QList<uint16_t>& midiKeys);
/// Switched off in Settings > Controllers: on again, with the mapping the
/// settings remember for it (or none), so the wizard can hear it.
void switchOn(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController);
/// Back to the original mapping ("(yours)" stays in the folder).
bool backToOriginal(UserSettingsPointer pConfig, ControllerManager* pManager, ::Controller* pController);

/// In a room, the wizard waits (your moves would go into the shared mix).
void setInRoom(bool inRoom);
bool inRoom();

/// The Settings page asks; the booth's strip (b2b::Controller) answers.
class Notifier : public QObject {
    Q_OBJECT
  public:
    static Notifier* instance();
  signals:
    /// Start the wizard on the booth for this controller.
    void startRequested(::Controller* pController);
    /// A mapping was saved or switched (the Settings page shows it anew).
    void mappingsChanged();
};

} // namespace mixxx::b2b::mapwizard
