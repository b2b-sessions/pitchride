#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>
#include <vector>

class ControlObject;
class ControlPotmeter;
class ControlProxy;
class ControlPushButton;
class EffectsManager;
class QWidget;

namespace mixxx::b2b {

class BeatFx;

/// The mixers of the B2B skins, where Mixxx has no control that does what the
/// mixer drawn in the skin does. All are in [B2BBooth]; other skins never
/// touch them.
///
/// A skin says which mixer it draws (a manifest attribute), and the effect
/// units are set up for that mixer:
///   layout   1 = club 4-channel with beat FX and color FX (Back2Back)
///            2 = 2-channel with one HPF / LPF filter (Back2Back Compact)
///            3 = 4+2 channel rotary with two filters and AUX sends
///                (Back2Back Club)
///            0 = none (another skin): the effect units are left alone
///
/// Layout 1:
///   beatfx_select   0..13, the beat FX dial (DELAY .. MOBIUS), loads the
///                   nearest Mixxx effect into effect unit 1
///   beatfx_ch       1..4 channel, 5 mic, 6 / 7 crossfader A / B, 8 master
///   color_fx        0 off, 1 SPACE, 2 DUB ECHO, 3 CRUSH, 4 SWEEP, 5 NOISE,
///                   6 FILTER (pressing the lit one turns it off)
///   colorN          channel N's COLOR knob, -1 .. 1, centre = no effect; it
///                   follows a controller's filter knob (the quick effect's
///                   super1)
///   color_param     the COLOR FX PARAMETER knob
///   mic_fx          0 off, 1 ECHO, 2 PITCH, 3 MEGAPHONE (effect unit 3);
///                   MIC REVERB is effect unit 4
///   master_cue      the master in the headphones
///   center_lock     CENTER LOCK: the COLOR knobs catch at the centre (turned
///                   quickly past it, they stop there; a little more turning
///                   leaves it)
///
/// Any layout:
///   hw_head_mix     a controller's HEADPHONES MIXING knob (0 CUE .. 1
///                   MASTER). When the headphones are on that controller's
///                   sound card (a DDJ-400 blends them itself) Mixxx's own
///                   blend stays at CUE; otherwise it follows the knob.
///   head_mix_shown  where the skins' MIX knobs are drawn (-1 .. 1)
///
/// Layouts 2 and 3, the filters (filter K is effect unit K):
///   filterK_mode    1 HPF, 2 BPF, 3 LPF (layout 2: pressing the lit one
///                   turns the filter off, 0). FREQ is the unit's super1.
///   filterK_lfo_on, filterK_depth (how far the LFO sweeps), filterK_lfo
///   (its rate), filterK_x2 (double speed), filterK_tap (tap the LFO tempo;
///   until then it follows the sync leader), filterK_beat (lit on the LFO's
///   beat)
///   filterK_xfader  VCF TO XFADER: the crossfader sweeps the cutoff
///   vcf_link        filter 2's cutoff follows filter 1's
///   auxA_<input>    layout 3: AUX A send (A = 1: effect unit 3, 2: unit 4)
///                   for an input ("Channel1".."Channel4", "Microphone",
///                   "Microphone2"): the input is sent while it is up
///   filter_assignN  layout 3: channel N through 0 none, 1 filter 1, 2 filter 2
///   booth_mute      layout 3: booth output off
class MixerControls : public QObject {
    Q_OBJECT
  public:
    explicit MixerControls(int decks = 4, QObject* pParent = nullptr);
    ~MixerControls() override;

    void setEffectsManager(std::shared_ptr<EffectsManager> pEffectsManager);
    /// The window with the skin (the BEAT FX display and X-PAD go into it).
    void setWindow(QWidget* pWindow);
    /// Reset the room (the club mixer): COLOR FX off with every COLOR knob
    /// in the middle, MIC FX off, BEAT FX off and back to its defaults (its
    /// dial and channel too). The channels' own knobs and faders are
    /// Mixxx's (the controller resets those).
    void resetMixer();
    void setSettingsPath(const QString& path) {
        m_settingsPath = path;
        watchHeadMix();
    }

  private:
    struct Filter {
        std::unique_ptr<ControlObject> mode;
        std::unique_ptr<ControlPushButton> lfoOn;
        std::unique_ptr<ControlPotmeter> lfoRate;
        std::unique_ptr<ControlPotmeter> depth;   // how far the LFO sweeps
        std::unique_ptr<ControlPushButton> xfader; // VCF TO XFADER
        std::unique_ptr<ControlPushButton> x2;
        std::unique_ptr<ControlPushButton> tap;
        std::unique_ptr<ControlObject> beat;
        std::unique_ptr<ControlProxy> freq; // the unit's super1
        double phase = 0;
        double tapBpm = 0;
        QElapsedTimer tapClock;
        int taps = 0;
    };

    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

    void layoutChanged(int layout);
    void setUpLayout();
    void loadEffect(const QString& chainGroup, int chainIndex, const char* effectId, bool clearOthers = true);

    // Layout 1
    void beatFxTurned(double value);
    void loadBeatFx(int position);
    void applyBeatFxChannel(int channel);
    void colorFxRequested(double value);
    void applyColorFx();
    void applyColor(int channel);
    void applyColorParam();
    void setQuickSuper(int channel, double value);
    void micFxRequested(double value);
    void applyMicFx();
    void applyMasterCue(bool on);
    /// CENTER LOCK on a COLOR knob's new value; relative: from the skin
    /// (moves start from where the knob is now), else a controller's
    /// absolute knob.
    double lockColor(int channel, double value, bool relative);
    void hwHeadMixTurned(double value);
    bool headphonesMixedByController();
    void watchHeadMix();

    // Layouts 2 and 3
    void filterModeRequested(int k, double value);
    void applyFilterMode(int k);
    void filterTap(int k);
    void lfoTick();
    void applyFilterAssign(int channel);
    /// Where filter k's cutoff is (0..1): its FREQ, or the crossfader (VCF
    /// TO XFADER), or filter 1's (VCF LINK, filter 2).
    double filterBase(int k) const;
    /// The cutoff, plus an LFO offset, to the filter's effect.
    void applyFilterMeta(int k, double offset = 0);
    void applyAuxSend(int index);

    int m_numDecks;
    int m_layout = 0;
    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::unique_ptr<ControlObject> m_layoutControl;

    std::unique_ptr<ControlPotmeter> m_beatFxSelect;
    std::unique_ptr<BeatFx> m_beatFx; // the BEAT FX section
    std::unique_ptr<ControlObject> m_beatFxChannel;
    QTimer m_beatFxSettle;
    int m_beatFxLoaded = -1;
    std::unique_ptr<ControlObject> m_colorFx;
    std::vector<std::unique_ptr<ControlPotmeter>> m_color;
    std::unique_ptr<ControlPotmeter> m_colorParam;
    std::vector<std::unique_ptr<ControlProxy>> m_quickSuper; // a controller's filter knobs
    std::vector<double> m_quickSuperSet;                     // what we last set them to
    std::unique_ptr<ControlObject> m_micFx;
    std::unique_ptr<ControlPushButton> m_masterCue;
    std::unique_ptr<ControlPushButton> m_centerLock;
    struct Lock {
        bool held = false;
        double push = 0; // turning since it caught
        double last = 0;
        QElapsedTimer since; // when it caught
    };
    std::vector<Lock> m_lock;
    std::unique_ptr<ControlPotmeter> m_hwHeadMix;
    std::unique_ptr<ControlPotmeter> m_headMixShown;
    std::unique_ptr<ControlProxy> m_headMix;
    bool m_settingHeadMix = false;
    QString m_settingsPath;
    QElapsedTimer m_headphonesChecked;
    bool m_headphonesOnController = false;

    Filter m_filters[2];
    std::unique_ptr<ControlPushButton> m_vcfLink;
    std::vector<std::unique_ptr<ControlPushButton>> m_filterAssign;
    std::unique_ptr<ControlPushButton> m_boothMute;
    double m_boothGainBefore = 1;
    std::unique_ptr<ControlProxy> m_clockBpm;
    std::unique_ptr<ControlProxy> m_crossfader;
    // AUX sends (layout 3): auxA_<input>, A = 1 (effect unit 3) or 2 (unit 4)
    std::vector<std::unique_ptr<ControlPotmeter>> m_auxSend;
    std::vector<std::pair<int, QString>> m_auxSendTo; // unit index, input group
    QTimer m_lfoTimer;
    QElapsedTimer m_lfoClock;
};

} // namespace mixxx::b2b
