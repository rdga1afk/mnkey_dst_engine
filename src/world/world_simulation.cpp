#include <monkey_dust/world/world_simulation.h>
#include <monkey_dust/world/world_events.h>
#include <monkey_dust/world/shop_inventory.h>
#include <monkey_dust/world/faction_system.h>
#include <monkey_dust/building/power_grid.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/platform/md_log.h>

// 1 Hz world-state tick. Call with LOGIC_TICK_S (0.1f) each logic tick.
// Economy rules (per 1-second fire):
//   Passive gold generation: +1/tick baseline
//   Trade routes: from loses `volume` gold, to gains `volume/2`
//   Prosperity: rises if gold>0, falls if gold==0
//   Aggression: inversely tracks prosperity
//   Population: grows +1 every 10 sim ticks when prosperity>128 (P11)
//              decays -1 every 10 sim ticks when aggression>200   (P11)
//   Raids: attacker steals (defender.gold*strength/255) gold      (P12)
void WorldSimulation::Tick(float delta_s) noexcept {
    accum_s_ += delta_s;
    if (accum_s_ < 1.0f) return;
    accum_s_ -= 1.0f;

    ++tick_count_;

    // Used for: faction territory recalculation, long-range spawn decisions.
    // BT_LOD_ULTRA_MODULO=960 at 10TPS → 96s; at 1Hz (WorldSim) → 960 ticks = 16 min.
    static constexpr uint32_t WS_ULTRA_RARE_TICKS = 960u;
    if (tick_count_ % WS_ULTRA_RARE_TICKS == 0u) {
        // Ultra-rare: rebuild faction territory maps, reset offscreen spawn cooldowns.
        // (Stub: full implementation in faction territory system.)
    }

    // P12: Process pending raid events
    for (int r = 0; r < WS_MAX_RAIDS; ++r) {
        FactionRaidEvent& raid = pending_raids_[r];
        if (!raid.active) continue;
        FactionState* attacker = nullptr;
        FactionState* defender = nullptr;
        for (int i = 0; i < faction_count_; ++i) {
            if (factions_[i].faction_id == raid.attacker_faction) attacker = &factions_[i];
            if (factions_[i].faction_id == raid.defender_faction) defender = &factions_[i];
        }
        if (attacker && defender) {
            uint16_t loot = (uint16_t)(((uint32_t)defender->gold * raid.strength) / 255u);
            if (loot > defender->gold) loot = defender->gold;
            defender->gold -= loot;
            uint32_t gain = (uint32_t)attacker->gold + loot;
            attacker->gold = (uint16_t)(gain < 0xFFFFu ? gain : 0xFFFFu);
        }
        raid.active = 0;  // consume
    }

    // F3: Economy — dynamic trade routes with prosperity-scaled efficiency.
    for (int r = 0; r < route_count_; ++r) {
        TradeRoute& rt = routes_[r];
        if (!rt.active || rt.volume == 0) continue;
        FactionState* src = nullptr;
        FactionState* dst = nullptr;
        for (int i = 0; i < faction_count_; ++i) {
            if (factions_[i].faction_id == rt.from_faction) src = &factions_[i];
            if (factions_[i].faction_id == rt.to_faction)   dst = &factions_[i];
        }
        if (!src || !dst) continue;
        // Route blocked if aggression between parties is high (conflict check)
        if (src->aggression > 180u || dst->aggression > 180u) {
            rt.active = 0; // war closes trade route
            continue;
        }
        if (src->gold >= rt.volume) {
            src->gold = (uint16_t)(src->gold - rt.volume);
            // Destination prosperity boosts return: 50% base + 0-25% prosperity bonus
            uint32_t bonus  = (uint32_t)dst->prosperity * rt.volume / 512u;
            uint32_t gain   = rt.volume / 2u + bonus;
            src->gold = (uint16_t)((uint32_t)src->gold + gain < 0xFFFFu
                                   ? (uint32_t)src->gold + gain : 0xFFFFu);
            // Destination prosperity rises from incoming trade
            if (dst->prosperity < 255) ++dst->prosperity;
        }
        // Closed route if src runs dry too often — auto-downscale volume
        else if (rt.volume > 1 && (tick_count_ % 5u) == 0u) {
            --rt.volume;
        }
    }

    // Update faction economy
    for (int i = 0; i < faction_count_; ++i) {
        FactionState& f = factions_[i];
        // Passive income scales with population (more people = more production)
        uint8_t income = (uint8_t)(1u + f.population / 64u);
        if ((uint32_t)f.gold + income < 0xFFFFu) f.gold = (uint16_t)(f.gold + income);
        else f.gold = 0xFFFFu;
        if (f.gold > 0 && f.prosperity < 255) ++f.prosperity;
        else if (f.gold == 0 && f.prosperity > 0) --f.prosperity;
        if (f.prosperity < 32u && f.aggression < 255) ++f.aggression;
        else if (f.prosperity > 32u && f.aggression > 0)  --f.aggression;
        // P11: population dynamics
        if ((tick_count_ % 10u) == 0u) {
            if (f.prosperity > 128u && f.population < 255) ++f.population;
            if (f.aggression > 200u && f.population > 0)  --f.population;
        }
    }

    // WageSystem: pay NPC wages once per game-day (every TICKS_PER_GAME_DAY ticks).
    // B-2: faction relation decay once per game-day → relations drift toward neutral.
    if ((tick_count_ % TICKS_PER_GAME_DAY) == 0u) {
        PayDailyWages();
        auto& fs = FactionSystem::Get();
        for (int a = 0; a < faction_count_; ++a) {
            for (int b = a + 1; b < faction_count_; ++b) {
                uint32_t fa = factions_[a].faction_id;
                uint32_t fb = factions_[b].faction_id;
                int8_t r = fs.GetRelation(fa, fb);
                if (r > 0) fs.ModRelation(fa, fb, -1);
                else if (r < 0) fs.ModRelation(fa, fb,  1);
            }
        }
    }

    // E-1: ShopInventory restock — advance restock_timer_s for all shops each logic second.
    // ShopInventory::TickRestock(delta_s) returns true when shop restocks; we call once/s.
    {
        auto& reg = MdRegistry::Get();
#if defined(MD_ECS_GAIA)
        static auto q_p3_world_simulation_1 = reg.Raw().query().all<ShopInventory&>();
#else
        static auto q_p3_world_simulation_1 = reg.Raw().query<ShopInventory>();
#endif
        MdEach(q_p3_world_simulation_1, [](ShopInventory& shop) {
            shop.TickRestock(1.0f);  // 1s per WorldSimulation tick
        });
    }

    // B-1: PowerGrid fuel consumption — drain fuel_amounts per registered consumer.
    PowerGrid::Get().Tick(1.0f);

    // F4: World event generation — every 30 sim ticks (~30s real-time).
    // Raids when aggression high, caravans when prosperity high.
    if ((tick_count_ % 30u) == 0u && faction_count_ >= 2) {
        auto& bus = WorldEventBus::Get();
        for (int i = 0; i < faction_count_; ++i) {
            const FactionState& src = factions_[i];
            // Raid: high aggression + has gold to fund it
            if (src.aggression > 160u && src.gold > 20u && faction_count_ > 1) {
                // Pick a random different faction as target
                int ti = (i + 1) % faction_count_;
                WorldEvent ev{};
                ev.type         = WorldEventType::Raid;
                ev.from_faction = src.faction_id;
                ev.to_faction   = factions_[ti].faction_id;
                ev.strength     = (uint8_t)((src.aggression - 100u) / 10u + 1u);
                bus.Push(ev);
            }
            // Caravan: high prosperity + active trade
            if (src.prosperity > 180u && src.gold > 50u) {
                WorldEvent ev{};
                ev.type         = WorldEventType::Caravan;
                ev.from_faction = src.faction_id;
                ev.strength     = (uint8_t)(src.prosperity / 64u);
                bus.Push(ev);
            }
        }
    }
}

// WageSystem: deduct NPC wages from faction gold once per game-day.
// Wages = population × BASE_WAGE_PER_DAY × wage_class_mult.
// If faction cannot afford wages: prosperity -5, aggression +2.
void WorldSimulation::PayDailyWages() noexcept {
    for (int i = 0; i < faction_count_; ++i) {
        FactionState& f = factions_[i];
        if (f.population == 0) continue;
        float mult    = WAGE_CLASS_MULT[wage_class_[i] < 3 ? wage_class_[i] : 0];
        uint32_t cost = (uint32_t)((float)f.population * BASE_WAGE_PER_DAY * mult);
        if (cost > 0xFFFFu) cost = 0xFFFFu;
        if (f.gold >= (uint16_t)cost) {
            f.gold -= (uint16_t)cost;
        } else {
            f.gold = 0;
            f.prosperity = (f.prosperity > 5u) ? (uint8_t)(f.prosperity - 5u) : 0u;
            f.aggression = (f.aggression < 253u) ? (uint8_t)(f.aggression + 2u) : 255u;
        }
    }
}

// P12: Queue a one-shot raid event
void WorldSimulation::QueueRaid(uint8_t attacker, uint8_t defender,
                                uint8_t strength) noexcept {
    for (int i = 0; i < WS_MAX_RAIDS; ++i) {
        if (!pending_raids_[i].active) {
            pending_raids_[i] = {attacker, defender, strength, 1};
            return;
        }
    }
    MD_LOG(MD_LOG_WARNING, "WorldSimulation: raid queue full");
}
