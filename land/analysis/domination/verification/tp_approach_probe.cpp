// Thin adapter to forcedCutPath. The Python runner checks real-state outcomes
// with the original Game.step. No game transition is implemented in this file.
#define LAND_TEST
#define TELEPORT_TRACE 0
#define HOME_RESCUE_TRACE 0
#define ENDGAME_TRACE 0
#define PREFIX_RETURN_TRACE 0
#define RETURN_ANCHOR_TRACE 0
#ifndef BOT_SOURCE
#error Compile with BOT_SOURCE pointing to the single-file bot under test.
#endif
#include BOT_SOURCE

// Lift the same local position onto a 48x48 four-player board. The two added
// players are far from the interception; they cannot create the tested collision.
static std::unique_ptr<land::Bot> fourPlayers(const land::Bot& original) {
    auto remap = [&](int k) { return (k / original.w) * 48 + k % original.w; };
    std::array<int, 4> spawn{remap(original.spawn[0]), remap(original.spawn[1]),
                             5 * 48 + 43, 42 * 48 + 43};
    auto bot = std::make_unique<land::Bot>(48, 48, 4, original.me, original.turns, spawn);
    bot->turn = original.turn;
    for (int k = 0; k < original.cells; ++k) {
        bot->owner[remap(k)] = original.owner[k];
        bot->trail[remap(k)] = original.trail[k];
    }
    for (int i = 0; i < 2; ++i) {
        bot->p[i] = original.p[i];
        bot->p[i].pos = remap(original.p[i].pos);
    }
    for (int i = 2; i < 4; ++i) {
        bot->p[i].pos = spawn[i]; bot->p[i].dir = i - 2; bot->p[i].area = 25;
        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx)
            bot->owner[spawn[i] + dy * 48 + dx] = i;
    }
    bot->fields();
    bot->idle.fill(0);
    bot->teleportPhase = 2;
    return bot;
}

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    int cases;
    if (!(std::cin >> cases)) return 1;
    for (int n = 0; n < cases; ++n) {
        std::string token, id;
        char wantedChar;
        int w, h, turns, players, me, moveMs, initMs;
        std::cin >> token >> id >> wantedChar;
        std::cin >> token >> w >> h >> turns >> players >> me >> moveMs >> initMs;
        if (players != 2) return 2;
        int wanted = int(std::string(land::DIRS).find(wantedChar));
        if (wanted < 0 || wanted >= 4) return 3;
        std::array<int, 4> spawn{};
        for (int i = 0; i < players; ++i) {
            int player, x, y; std::cin >> token >> player >> x >> y;
            spawn[player] = y * w + x;
        }
        std::cin >> token;
        std::array<int, 4> initial{};
        for (int i = 0; i < players; ++i) {
            char d; std::cin >> d;
            initial[i] = int(std::string(land::DIRS).find(d));
        }
        auto base = std::make_unique<land::Bot>(w, h, players, me, turns, spawn);
        base->p[me].dir = initial[me];
        base->prepareSpawn();
        if (!base->read()) return 4;
        base->fields();
        base->idle.fill(0);
        base->teleportPhase = 2;
        int victim = 1 - me;
        bool comma = false;
        std::cout << "{\"id\":\"" << id << "\",\"trials\":[";
        auto emit = [&](const char* name, land::Bot& bot, int targetVictim) {
            auto route = bot.forcedCutPath(targetVictim);
            int v = bot.next[bot.p[me].pos][wanted];
            bool validVictim = targetVictim >= 0 && targetVictim < bot.players && targetVictim != me;
            if (comma) std::cout << ',';
            comma = true;
            std::cout << "{\"name\":\"" << name << "\",\"path\":\"";
            for (int d : route) {
                if (d < 0 || d >= 4) { std::cout << '?'; continue; }
                std::cout << land::DIRS[d];
            }
            std::cout << "\",\"players\":" << bot.players << ",\"phase\":" << bot.teleportPhase
                      << ",\"own_length\":" << bot.p[me].length
                      << ",\"victim\":" << targetVictim
                      << ",\"victim_length\":" << (validVictim ? bot.p[targetVictim].length : -1)
                      << ",\"victim_idle\":" << (validVictim ? bot.idle[targetVictim] : -1)
                      << ",\"enemy_home\":" << (validVictim ? bot.enemyHome[targetVictim] : -1)
                      << ",\"remaining\":" << bot.turns - bot.turn
                      << ",\"first_legal\":" << (v >= 0 && wanted != (bot.p[me].dir ^ 1))
                      << ",\"first_collision\":" << bot.collision(wanted)
                      << ",\"first_owner\":" << (v >= 0 ? bot.owner[v] : -2) << '}';
        };
        auto trial = [&](const char* name, int targetVictim, auto mutate) {
            // Rebuild initialized state instead of copying unused scratch
            // arrays whose values the production bot has not initialized.
            auto bot = std::make_unique<land::Bot>(w, h, players, me, turns, spawn);
            bot->p[me].dir = initial[me]; bot->prepareSpawn();
            bot->p = base->p; bot->turn = base->turn;
            std::copy(base->owner, base->owner + base->cells, bot->owner);
            std::copy(base->trail, base->trail + base->cells, bot->trail);
            bot->fields(); bot->idle.fill(0); bot->teleportPhase = 2;
            mutate(*bot);
            emit(name, *bot, targetVictim);
        };
        auto unchanged = [](land::Bot&) {};
        trial("real", victim, unchanged);
        trial("phase-0", victim, [](land::Bot& b) { b.teleportPhase = 0; });
        trial("phase-1", victim, [](land::Bot& b) { b.teleportPhase = 1; });
        auto melee = fourPlayers(*base);
        emit("n4", *melee, victim);
        trial("own-line", victim, [&](land::Bot& b) {
            b.owner[b.p[me].pos] = -1; b.trail[b.p[me].pos] = me; b.p[me].length = 1;
        });
        trial("first-nonown", victim, [&](land::Bot& b) {
            b.owner[b.next[b.p[me].pos][wanted]] = -1;
        });
        trial("home-1", victim, [&](land::Bot& b) { b.enemyHome[victim] = 1; });
        trial("last-1", victim, [](land::Bot& b) { b.turn = b.turns - 1; });
        trial("first-reverse", victim, [&](land::Bot& b) { b.p[me].dir = wanted ^ 1; });
        trial("victim-cleared", victim, [&](land::Bot& b) {
            for (int k = 0; k < b.cells; ++k) if (b.trail[k] == victim) b.trail[k] = -1;
            b.p[victim].length = 0; b.enemyHome[victim] = 0;
        });
        std::cout << "]}\n";
    }
    return std::cin ? 0 : 5;
}
