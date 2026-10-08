#pragma once

#include <memory>
#include <string>

#include <cge/GameApi.hpp>

namespace cge {

// A copy of a game's GameInfo (the strings are copied, so they stay valid
// even after the library is unloaded).
struct GameMetadata {
    std::string name;
    std::string description;
    int width = 0;
    int height = 0;
    int minPlayers = 1;
    int maxPlayers = 1;
    int ticksPerSecond = 30;
};

// Checks that the numbers a game declares are sensible. Returns false and
// fills `error` otherwise.
bool validateGameInfo(const GameInfo& info, std::string& error);

// One loaded game plugin (a shared library: .so on Linux, also used on macOS).
//
// Loading works like this:
//   dlopen("tictactoe.so")              open the library
//   dlsym(handle, "cge_create_game")    find the C function by name
//   create() -> cge::Game*              the game object we talk to
//
// The library stays loaded as long as anybody holds the shared_ptr - so a
// running session keeps "its" version even if the game is rebuilt meanwhile.
class GameLibrary {
public:
    static std::shared_ptr<GameLibrary> load(const std::string& path, std::string& error);
    ~GameLibrary();

    GameLibrary(const GameLibrary&) = delete;
    GameLibrary& operator=(const GameLibrary&) = delete;

    const GameMetadata& info() const { return info_; }
    const std::string& path() const { return path_; }

    Game* create() const { return create_(); }
    void destroy(Game* game) const { destroy_(game); }

private:
    GameLibrary() = default;

    void* handle_ = nullptr;
    std::string path_;
    GameMetadata info_;
    Game* (*create_)() = nullptr;
    void (*destroy_)(Game*) = nullptr;
};

// Runs a game for a few seconds of simulated play with fake players and
// random input. Used in a separate process ("--validate-game"), so that a
// game that crashes or hangs is rejected without harming the server.
// Prints a JSON summary and returns the process exit code (0 = game is OK).
int runGameValidation(const std::string& libraryPath);

}  // namespace cge
