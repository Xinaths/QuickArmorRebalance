#include "Enchantments.h"

#include "Config.h"
#include "Data.h"

namespace {
    using namespace QuickArmorRebalance;

    bool ShouldEnchant(RE::TESBoundObject* obj, const EnchantProbability* contParams, int level) {
        auto params = MapFind(g_Data.enchParams, obj);
        if (!params) {
            // logger::info("No ench params: {}", obj->GetName());
            return false;
        }

        if (obj->IsArmor()) {
            auto armor = obj->As<RE::TESObjectARMO>();
            if (armor->formEnchanting) return false;
        } else if (obj->IsWeapon()) {
            auto weapon = obj->As<RE::TESObjectWEAP>();
            if (weapon->formEnchanting) return false;
        } else {
            return false;
        }

        float chance = g_Config.enchChanceBase + std::min(g_Config.enchChanceBonus * (level - params->level), g_Config.enchChanceBonusMax);
        chance *= (0.01f * g_Config.fEnchantRates) * contParams->enchRate * params->base.enchRate * params->unique.enchRate;

        return RNG_f() < chance;
    }

    inline float PowerCalc(const ObjEnchantParams* params, const EnchantProbability* contParams, int level) {
        return contParams->enchPower * params->base.enchPower * params->unique.enchPower * std::lerp((float)params->level, (float)level, RNG_f()) *
               std::normal_distribution<float>(1.0f, 0.25f)(RNG);
    }

    RE::EnchantmentItem* PickEnchantStrength(RE::EnchantmentItem* ench, const ObjEnchantParams* params, const EnchantProbability* contParams, int level, int& charge) {
        charge = (int)std::lerp(g_Config.enchWeapChargeMin, g_Config.enchWeapChargeMax, std::clamp(PowerCalc(params, contParams, level) / g_Config.levelMaxDist, 0.0f, 1.0f));

        const auto& ranks = g_Config.mapEnchantments[ench];
        if (ranks.ranks.empty()) return nullptr;
        if (ranks.ranks.size() == 1) return ranks.ranks.front();

        float power = PowerCalc(params, contParams, level);

        power = (power - ranks.levelMin) / (std::min(g_Config.levelMaxDist, ranks.levelMax) - ranks.levelMin);
        if (power <= 0) return ranks.ranks.front();
        if (power >= 1.0f) return ranks.ranks.back();

        return ranks.ranks[(int)(power * (ranks.ranks.size() - 1))];  //-1 because max rank is handled at power>=1.0
    }

    RE::EnchantmentItem* PickEnchant(RE::TESBoundObject* obj, const EnchantProbability* contParams, int level, int& charge) {
        auto params = MapFind(g_Data.enchParams, obj);
        if (!params) {
            // logger::info("{}: No params found", obj->GetName());
            return nullptr;
        }

        auto pool = params->base.enchPool;
        if (!pool) {
            // logger::info("{}: No pool found", obj->GetName());
            return nullptr;
        }

        if (params->unique.enchPool && RNG_f() <= params->uniquePoolChance) pool = params->unique.enchPool;

        std::vector<std::pair<RE::EnchantmentItem*, float>> mapWeights;
        mapWeights.reserve(pool->enchs.size());

        if (obj->IsArmor()) {
            for (auto& i : pool->enchs) {
                auto& ranks = g_Config.mapEnchantments[i.first];
                if (level < ranks.levelMin) continue;

                auto e = i.first;
                if (e->data.castingType != RE::MagicSystem::CastingType::kConstantEffect || e->data.delivery != RE::MagicSystem::Delivery::kSelf) {
                    continue;
                }

                if (e->data.wornRestrictions) {
                    if (!obj->HasKeywordInList(e->data.wornRestrictions, false)) continue;
                }

                mapWeights.emplace_back(i.first, i.second);
            }
        } else if (obj->IsWeapon()) {
            for (auto& i : pool->enchs) {
                auto& ranks = g_Config.mapEnchantments[i.first];
                if (level < ranks.levelMin) continue;

                auto e = i.first;
                if (e->data.castingType != RE::MagicSystem::CastingType::kFireAndForget || e->data.delivery != RE::MagicSystem::Delivery::kTouch) {
                    continue;
                }

                mapWeights.emplace_back(i.first, i.second);
            }
        }

        if (mapWeights.empty()) {
            // logger::info("{}: No enchantments", obj->GetName());
            return nullptr;
        }

        // logger::info("{}: Enchantments", obj->GetName());
        // for (auto& i : mapWeights) logger::info("- {}", i.first->GetName());

        float total = 0.0f;
        for (auto& i : mapWeights) total += i.second;

        auto pick = total * ((double)RNG() / (double)RNG.max());
        for (auto& i : mapWeights) {
            pick -= i.second;
            if (pick <= 0) {
                return PickEnchantStrength(i.first, params, contParams, level, charge);
            }
        }

        return nullptr;
    }

    void AddEnchantments(RE::TESObjectREFR* a_this) {
        if (!g_Config.bEnableEnchantmentDistrib) return;

        if (!a_this->GetBaseObject()) return;
        auto cont = a_this->GetBaseObject()->As<RE::TESObjectCONT>();
        if (!cont) return;

        auto contEnchChance = MapFind(g_Data.distContainers, cont);
        if (!contEnchChance) return;

        // logger::info("{} level={}", a_this->GetName(), a_this->GetCalcLevel(true));

        int level = a_this->GetCalcLevel(true);
        level -= g_Config.levelEnchDelay;
        if (level < 1) return;

        for (auto& item : a_this->GetInventory()) {
            if (g_Data.distItems.contains(item.first)) {
                // logger::info("- Has: {} x{}", item.first->GetName(), item.second.first);

                auto entry = item.second.second.get();
                if (entry->extraLists) {
                    if (true) {
                        for (auto& ls : *entry->extraLists) {
                            if (ls->HasType(RE::ExtraEnchantment::EXTRADATATYPE)) continue;

                            if (ShouldEnchant(entry->object, contEnchChance, level)) {
                                int charge = 0;
                                auto ench = PickEnchant(entry->object, contEnchChance, level, charge);

                                if (ench) {
                                    // logger::info("Adding {} to {}", ench->GetName(), item.first->GetName());

                                    ls->Add(new RE::ExtraEnchantment(ench, item.first->IsWeapon() ? (uint16_t)charge : 0));
                                    if (item.first->IsWeapon() && g_Config.bEnchantRandomCharge) {
                                        if (!ls->HasType(RE::ExtraCharge::EXTRADATATYPE)) {
                                            auto pExtra = new RE::ExtraCharge();
                                            pExtra->charge = charge * std::powf(RNG_f(), 0.33f);
                                            ls->Add(pExtra);                                     
                                        }
                                    }
                                }
                            }

                            /*
                            logger::info("-- Extra list start");
                            for (auto& extra : *ls) {
                                if (extra.GetType() == RE::ExtraDataType::kLeveledItem) {
                                    auto p = static_cast<RE::ExtraLeveledItem*>(&extra);
                                    logger::info("---- Extra: {:#10x}", p->levItem);
                                }
                            }
                            */
                        }
                    }
                } else {
                    // logger::info("No extra list: {}", item.first->GetName());
                }
            }
        }
    }

    // Hook found from Container Distribution Framework
    //  https://www.nexusmods.com/skyrimspecialedition/mods/120152
    //  https://github.com/SeaSparrowOG/DynamicContainerInventoryFramework

    struct TESObjectREFR_Initialize {
        // Might be able to use TESObjectREFR::InitItemImpl virtual func instead
        static constexpr auto id = REL::VariantID(19105, 19507, 19105);
        static constexpr auto offset = REL::VariantOffset(0x69A, 0x78C, 0x69A);

        static void thunk(RE::TESObjectREFR* a_this, bool a3) {
            // logger::info("TESObjectREFR_Initialize");

            func(a_this, a3);

            AddEnchantments(a_this);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct TESObjectREFR_Reset {
        // Might be able to use TESObjectREFR::ResetInventory virtual func instead
        static constexpr auto id = REL::VariantID(19375, 19802, 19375);
        static constexpr auto offset = REL::VariantOffset(0x145, 0x12B, 0x145);

        static void thunk(RE::TESObjectREFR* a_this, bool a3) {
            // logger::info("TESObjectREFR_Reset");

            func(a_this, a3);

            AddEnchantments(a_this);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
}

void QuickArmorRebalance::InstallEnchantmentHooks() {
    if (!g_Config.bEnableEnchantmentDistrib) return;

    static bool bOnce = false;
    if (bOnce) return;
    bOnce = true;

    SKSE::AllocTrampoline(14 * 2);
    write_thunk_call<TESObjectREFR_Initialize>();
    write_thunk_call<TESObjectREFR_Reset>();
}

float EnchCmp(RE::EnchantmentItem* a, RE::EnchantmentItem* b) {
    for (auto effect : a->effects) {
        bool bFound = false;
        for (auto effect2 : b->effects) {
            if (effect->baseEffect == effect2->baseEffect) {
                float cmp = effect->effectItem.magnitude - effect2->effectItem.magnitude;
                if (cmp != 0.0f) return cmp;
                bFound = true;
                break;
            }
        }

        if (!bFound) return 1;
    }
    return 0.0f;
}

void QuickArmorRebalance::FinalizeEnchantmentConfig() {
    // Delete empty leveled enchantments
    for (auto& pool : g_Config.mapEnchPools) {
        std::erase_if(pool.second.enchs, [](const auto& i) {
            auto it = g_Config.mapEnchantments.find(i.first);
            if (it == g_Config.mapEnchantments.end()) return true;
            if (it->second.ranks.empty()) return true;

            return false;
        });

        std::vector<const char*> names;
        for (auto i : pool.second.enchs) names.push_back(i.first->GetName());

        std::sort(names.begin(), names.end(), [](const char* a, const char* b) { return _stricmp(a, b) < 0; });
        size_t nLen = 0;
        for (auto i : names) nLen += strlen(i) + 1;

        pool.second.strContents.reserve(nLen);
        for (auto i : names) {
            if (!pool.second.strContents.empty()) pool.second.strContents.append("\n");
            pool.second.strContents.append(i);
        }
    }

    // Sort by power & remove any duplicates
    for (auto& i : g_Config.mapEnchantments) {
        auto& ranks = i.second.ranks;
        std::sort(ranks.begin(), ranks.end(), [](auto a, auto b) { return EnchCmp(a, b) < 0.0f; });

        for (auto it = ranks.begin(); it != ranks.end(); it++) {
            auto itNext = it + 1;
            if (itNext != ranks.end()) {
                if (!EnchCmp(*it, *itNext)) {
                    ranks.erase(itNext);
                }
            }
        }

        /*
        logger::info("Ench pool: {}", i.first->fullName.c_str());
        for (auto ench : i.second) {
            logger::info("-- {}", ench->effects[0]->GetMagnitude());
        }
        */
    }
}
