// Read-only adapter to the selected bot. All expected outcomes come from Game.step
// in verify_mechanics.py; no game simulator is implemented in this adapter.
#define LAND_TEST
#define TELEPORT_TRACE 0
#define HOME_RESCUE_TRACE 0
#ifndef BOT_SOURCE
#error Compile with BOT_SOURCE pointing to the single-file bot under test.
#endif
#include BOT_SOURCE
#include <iomanip>

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::cout << std::setprecision(17);
    int cases;
    if (!(std::cin >> cases)) return 1;
    for (int n = 0; n < cases; ++n) {
        std::string token, id;
        int duel, reset, w, h, turns, players, me, moveMs, initMs;
        std::cin >> token >> id >> duel >> reset;
        std::cin >> token >> w >> h >> turns >> players >> me >> moveMs >> initMs;
        if (players != 2) return 2;
        std::array<int, 4> spawn{};
        for (int i = 0; i < players; ++i) {
            int player, x, y;
            std::cin >> token >> player >> x >> y;
            spawn[player] = y * w + x;
        }
        std::cin >> token;
        std::array<int, 4> initial{};
        for (int i = 0; i < players; ++i) {
            char d; std::cin >> d;
            initial[i] = int(std::string(land::DIRS).find(d));
        }
        auto bot = std::make_unique<land::Bot>(w, h, players, me, turns, spawn);
        // Snapshots can begin after turn zero. Initialize respawn orientation
        // from the original INIT metadata before restoring the current state.
        bot->p[me].dir = initial[me];
        bot->prepareSpawn();
        if (!bot->read()) return 3;
        bot->fields();
        bot->idle.fill(0); // Every frozen snapshot has two active players.
        std::cout << "{\"id\":\"" << id << '\"';
        if (duel) {
            auto maps = bot->duelCaptureMaps();
            std::cout << ",\"rescues\":{";
            bool comma = false, cacheCorrect = true;
            for (int d = 0; d < 4; ++d) if (d != (bot->p[me].dir ^ 1)) {
                if (comma) std::cout << ',';
                comma = true;
                int rescue = bot->rescueHome(d);
                std::cout << '\"' << land::DIRS[d] << "\":\"" << land::DIRS[rescue] << '\"';
                if (rescue != d) {
                    bot->plan = {d}; bot->teleportPhase = 2;
                    bot->teleportEnemy = 1 - me; bot->teleportReset = {d};
                    int finalized = bot->finalizeMove(d);
                    cacheCorrect &= finalized == rescue && bot->plan.empty() && bot->teleportPhase == 0
                                    && bot->teleportEnemy == -1 && bot->teleportReset.empty()
                                    && bot->expected == bot->next[bot->p[me].pos][rescue];
                }
            }
            std::cout << "},\"rescue_cache_correct\":" << cacheCorrect << ",\"matrix\":[";
            comma = false;
            for (int d = 0; d < 4; ++d) if (d != (bot->p[me].dir ^ 1))
                for (int z = 0; z < 4; ++z) if (z != (bot->p[1 - me].dir ^ 1)) {
                    int afterOwner[land::MAX_CELLS];
                    auto outcome = bot->resolveDuel(d, z, maps, afterOwner);
                    if (comma) std::cout << ',';
                    comma = true;
                    std::cout << "{\"mine\":\"" << land::DIRS[d] << "\",\"enemy\":\"" << land::DIRS[z]
                              << "\",\"areas\":[" << outcome.area[0] << ',' << outcome.area[1]
                              << "],\"dead\":[" << outcome.dead[0] << ',' << outcome.dead[1]
                              << "],\"captured\":[" << outcome.captured[0] << ',' << outcome.captured[1]
                              << "],\"owner\":\"";
                    for (int k = 0; k < bot->cells; ++k)
                        std::cout << (afterOwner[k] < 0 ? '.' : char('0' + afterOwner[k]));
                    std::cout << "\"}";
                }
            std::cout << ']';
        }
        if (reset) {
            auto options = bot->resetOptions();
            std::cout << ",\"weights\":[" << bot->weights[0] << ',' << bot->weights[1] << ',' << bot->weights[2]
                      << "],\"resets\":[";
            bool comma = false;
            for (const auto& option : options) {
                double simulatedValue = -1;
                int simulatedStep = bot->simulateReset(option.path, &simulatedValue);
                if (comma) std::cout << ',';
                comma = true;
                std::cout << "{\"route\":\"";
                for (int d : option.path) std::cout << land::DIRS[d];
                std::cout << "\",\"secured\":" << option.secured << ",\"simulated_step\":" << simulatedStep
                          << ",\"simulated_value\":" << simulatedValue << '}';
            }
            std::cout << ']';
        }
        std::cout << "}\n";
    }
}
