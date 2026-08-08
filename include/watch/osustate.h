#pragma once
#include <QMetaType>

namespace ovc::watch {

// osu! gamemodes, matching osu!'s own ruleset ids (and tosu's).
enum class Ruleset : int {
    Osu = 0,
    Taiko = 1,
    Fruits = 2,
    Mania = 3,
    Unknown = -1,
};

// Coarse game state. Values match osu! stable / tosu so the stable reader can
// cast the raw status int directly. Only the states we act on are named.
enum class GameState : int {
    Menu = 0,
    Edit = 1,
    Play = 2,
    Exit = 3,
    SelectEdit = 4,
    SelectPlay = 5,
    ResultScreen = 7,
    Lobby = 11,
    SelectMulti = 13,
    Unknown = -1,
};

} // namespace ovc::watch

// Passed through queued signals if the watcher ever moves off the GUI thread.
Q_DECLARE_METATYPE(ovc::watch::GameState)
Q_DECLARE_METATYPE(ovc::watch::Ruleset)
