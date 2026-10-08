#include "engine/GameLibrary.hpp"

#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <random>

#include "core/Json.hpp"

namespace cge {

bool validateGameInfo(const GameInfo& info, std::string& error) {
    if (info.name == nullptr || info.name[0] == '\0') {
        error = "the game has no name";
    } else if (info.width < 16 || info.width > 1920 || info.height < 16 || info.height > 1080) {
        error = "canvas size must be between 16x16 and 1920x1080 pixels";
    } else if (info.minPlayers < 1 || info.maxPlayers < info.minPlayers || info.maxPlayers > 16) {
        error = "player counts must satisfy 1 <= minPlayers <= maxPlayers <= 16";
    } else if (info.ticksPerSecond < 1 || info.ticksPerSecond > 120) {
        error = "ticksPerSecond must be between 1 and 120";
    } else {
        return true;
    }
    return false;
}

std::shared_ptr<GameLibrary> GameLibrary::load(const std::string& path, std::string& error) {
    // RTLD_NOW: resolve every symbol right away, so a broken library fails
    // here and not in the middle of a game. RTLD_LOCAL: keep its symbols
    // private, so two games can use the same class names.
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* reason = dlerror();
        error = std::string("cannot load library: ") + (reason ? reason : "unknown error");
        return nullptr;
    }

    auto version = reinterpret_cast<int (*)()>(dlsym(handle, "cge_api_version"));
    auto getInfo = reinterpret_cast<const GameInfo* (*)()>(dlsym(handle, "cge_game_info"));
    auto create = reinterpret_cast<Game* (*)()>(dlsym(handle, "cge_create_game"));
    auto destroy = reinterpret_cast<void (*)(Game*)>(dlsym(handle, "cge_destroy_game"));
    if (!version || !getInfo || !create || !destroy) {
        dlclose(handle);
        error = "the library does not export a game (did you forget CGE_EXPORT_GAME?)";
        return nullptr;
    }
    if (version() != kApiVersion) {
        dlclose(handle);
        error = "the game was built for SDK version " + std::to_string(version()) + ", the engine needs version " +
                std::to_string(kApiVersion);
        return nullptr;
    }
    const GameInfo* info = getInfo();
    if (info == nullptr || !validateGameInfo(*info, error)) {
        dlclose(handle);
        if (info == nullptr) error = "cge_game_info() returned nothing";
        return nullptr;
    }

    std::shared_ptr<GameLibrary> library(new GameLibrary());
    library->handle_ = handle;
    library->path_ = path;
    library->create_ = create;
    library->destroy_ = destroy;
    library->info_.name = info->name;
    library->info_.description = info->description ? info->description : "";
    library->info_.width = info->width;
    library->info_.height = info->height;
    library->info_.minPlayers = info->minPlayers;
    library->info_.maxPlayers = info->maxPlayers;
    library->info_.ticksPerSecond = info->ticksPerSecond;
    return library;
}

GameLibrary::~GameLibrary() {
    if (handle_ != nullptr) dlclose(handle_);
}

// =================================================================== validation

namespace {

// A pretend engine for the test run.
class FakeHost : public GameHost {
public:
    explicit FakeHost(int players) : players_(players) {}
    std::string playerName(int player) const override {
        return player >= 0 && player < players_ ? "Tester" + std::to_string(player + 1) : "";
    }
    bool playerConnected(int player) const override { return player >= 0 && player < players_ && connected_; }
    void log(const std::string&) override {}
    void setConnected(bool connected) { connected_ = connected; }

private:
    int players_;
    bool connected_ = true;
};

}  // namespace

int runGameValidation(const std::string& libraryPath) {
    auto fail = [](const std::string& message) {
        Json result = Json::object();
        result.set("ok", false).set("error", message);
        std::printf("%s\n", result.dump().c_str());
        return 1;
    };

    std::string error;
    std::shared_ptr<GameLibrary> library = GameLibrary::load(libraryPath, error);
    if (!library) return fail(error);
    const GameMetadata& info = library->info();

    Game* game = nullptr;
    double totalTickMs = 0;
    double slowestTickMs = 0;
    const int ticks = 300;  // 10 seconds of play at 30 ticks per second
    try {
        game = library->create();
        if (game == nullptr) return fail("cge_create_game() returned nothing");

        FakeHost host(info.maxPlayers);
        Canvas canvas(info.width, info.height);
        game->start(host);
        for (int player = 0; player < info.maxPlayers; ++player) game->playerJoined(player, host.playerName(player));

        // Throw lots of random input at the game: clicks, moves and keys,
        // including positions outside the canvas and unusual keys.
        std::mt19937 random(12345);
        const char* keys[] = {"1", "5", "9", "a", "r", "R", " ", "Enter", "ArrowUp", "ArrowLeft", "Escape", "?"};
        for (int tick = 0; tick < ticks; ++tick) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 3; ++i) {
                InputEvent event;
                event.kind = static_cast<InputKind>(1 + random() % 5);
                event.player = static_cast<int>(random() % static_cast<unsigned>(info.maxPlayers));
                event.x = static_cast<int>(random() % static_cast<unsigned>(info.width + 40)) - 20;
                event.y = static_cast<int>(random() % static_cast<unsigned>(info.height + 40)) - 20;
                event.key = keys[random() % (sizeof(keys) / sizeof(keys[0]))];
                game->input(event);
            }
            if (tick == 150) {  // a player drops out and comes back
                host.setConnected(false);
                game->playerLeft(0);
                host.setConnected(true);
                game->playerJoined(0, host.playerName(0));
            }
            game->update(1.0 / info.ticksPerSecond);
            game->render(canvas);
            (void)game->status();
            (void)game->metrics();
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            totalTickMs += ms;
            slowestTickMs = std::max(slowestTickMs, ms);
        }
        library->destroy(game);
        game = nullptr;
    } catch (const std::exception& exception) {
        return fail(std::string("the game threw an exception: ") + exception.what());
    } catch (...) {
        return fail("the game threw an unknown exception");
    }

    // A game that needs more time per tick than it has would lag forever.
    const double budgetMs = 1000.0 / info.ticksPerSecond;
    if (totalTickMs / ticks > budgetMs) {
        return fail("the game is too slow: " + std::to_string(totalTickMs / ticks) + " ms per tick, budget is " +
                    std::to_string(budgetMs) + " ms");
    }

    Json result = Json::object();
    result.set("ok", true)
        .set("name", info.name)
        .set("ticks", ticks)
        .set("averageTickMs", totalTickMs / ticks)
        .set("slowestTickMs", slowestTickMs);
    std::printf("%s\n", result.dump().c_str());
    return 0;
}

}  // namespace cge
