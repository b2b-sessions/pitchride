#pragma once

#include <QtGlobal>

/// The words in the "i" panels next to the decks and the mixer: the one
/// place to change them (and the links). Plain text; the link opens in the
/// browser. Brand names only, never their logos or artwork. Anything
/// beyond plain ASCII as UTF-8 bytes ("\xE2\x86\x92" is the arrow), so
/// every compiler reads it the same.
namespace mixxx::b2b::about {

struct Text {
    const char* tooltip;  // on the "i" button, and its accessible name
    const char* title;
    const char* body;
    const char* linkText;
    const char* url;
};

inline constexpr Text kDeck = {
        QT_TRANSLATE_NOOP("AboutGear", "About this deck"),
        QT_TRANSLATE_NOOP("AboutGear", "About this deck"),
        QT_TRANSLATE_NOOP("AboutGear",
                "This deck is modelled on the AlphaTheta CDJ-3000X, the latest flagship club "
                "player from the makers of Pioneer DJ, found in DJ booths worldwide. The layout "
                "and controls follow the real thing, so the skills you build here carry over "
                "when you step up to club equipment."),
        QT_TRANSLATE_NOOP("AboutGear", "View the CDJ-3000X on AlphaTheta \xE2\x86\x92"),
        "https://alphatheta.com/en/product/player/cdj-3000x/black/",
};

inline constexpr Text kMixer = {
        QT_TRANSLATE_NOOP("AboutGear", "About this mixer"),
        QT_TRANSLATE_NOOP("AboutGear", "About this mixer"),
        QT_TRANSLATE_NOOP("AboutGear",
                "This mixer is modelled on the Pioneer DJ DJM-A9, the 4-channel club-standard "
                "mixer found in DJ booths worldwide and the successor to the DJM-900NXS2. The "
                "layout and controls follow the real thing, so the skills you build here carry "
                "over when you step up to club equipment."),
        QT_TRANSLATE_NOOP("AboutGear", "View the DJM-A9 on Pioneer DJ \xE2\x86\x92"),
        "https://www.pioneerdj.com/en/product/dj-mixers/djm-a9/",
};

} // namespace mixxx::b2b::about
