#include "LootLists.h"

#include "ArmorChanger.h"
#include "ArmorSetBuilder.h"
#include "Config.h"
#include "Data.h"

/*//////////////////
Loot table notes

- Entry (container)
- Chance wrapper
- Level curve selection
- Level curve / armor class (iron ... daedric)
- Rarity selection (common / rare)
- Armor sets (set / color)
- Armor set items

*/

using namespace rapidjson;
using namespace QuickArmorRebalance;

namespace {
    int GetJsonInt(const Value& parent, const char* id, int min = 0, int max = 0, int d = 0) {
        if (parent.HasMember(id)) {
            const auto& v = parent[id];
            if (v.IsInt()) return std::clamp(v.GetInt(), min, max);
        }

        return std::max(min, d);
    }

    bool DoNotDistribute(RE::TESObjectARMO* armor) {
        for (auto addon : armor->armorAddons) {
            if (g_Data.loot->dynamicVariantsDAV.contains(addon)) return true;
        }

        for (const auto& pv : g_Config.mapPrefVariants) {
            switch (pv.second.pref) {
                case Pref_With:
                    if (g_Data.loot->prefVartWithout[pv.second.hash].contains(armor)) return true;
                    break;
                case Pref_Without:
                    if (g_Data.loot->prefVartWith[pv.second.hash].contains(armor)) return true;
                    break;
            }           
        }

        return false;
    }
}

Value QuickArmorRebalance::MakeLootChanges(const ArmorChangeParams& params, RE::TESBoundObject* i,
                                           MemoryPoolAllocator<>& al) {
    for (auto item : params.items) //Don't build loot sets from current armor (aka mixed mod sets) - maybe in the future
        if (item->GetFile(0) != i->GetFile(0)) return {};

    if (!params.isWornArmor && params.bDistribute && params.distProfile) {
        Value loot(kObjectType);

        if (auto armor = i->As<RE::TESObjectARMO>()) {
            if (params.bDistAsSet &&
                ((unsigned int)armor->GetSlotMask() & (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kBody) != 0) {
                if (params.bMatchSetPieces) {
                    auto s = BuildSetFrom(i, params.items);
                    if (!s.empty()) {
                        Value setv(kArrayType);
                        for (auto j : s) setv.PushBack(GetFileId(j), al);
                        loot.AddMember("set", setv, al);
                    }
                } else {
                    // If its not matching, we actually can just throw everything into one set and it'll be mixed
                    // properly on loading
                    if (!params.bMixedSetDone) {
                        Value setv(kArrayType);
                        for (auto j : params.items) setv.PushBack(GetFileId(j), al);
                        loot.AddMember("set", setv, al);
                        params.bMixedSetDone = true;
                    }
                }
            }

            if (params.bDistAsPieces) loot.AddMember("piece", true, al);
        } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
            if (params.bDistAsPieces) loot.AddMember("piece", true, al);       
        } else if (auto ammo = i->As<RE::TESAmmo>()) {
            if (params.bDistAsPieces) loot.AddMember("piece", true, al);
        }

        if (!loot.ObjectEmpty()) {
            loot.AddMember("profile", Value(params.distProfile, al), al);
            loot.AddMember("group", Value(params.armorSet->loot->name.c_str(), al), al);
            loot.AddMember("rarity", params.rarity, al);
            return loot;
        }
    }

    return Value();
}

void QuickArmorRebalance::LoadLootChanges(RE::TESBoundObject* item, const Value& jsonLoot) {
    if (!item) return;

    if (!jsonLoot.HasMember("profile")) return;
    const auto& jsonProfile = jsonLoot["profile"];
    if (!jsonProfile.IsString()) return;

    const auto& itProfile = g_Data.loot->distProfiles.find(jsonProfile.GetString());
    if (itProfile == g_Data.loot->distProfiles.end()) return;

    if (!jsonLoot.HasMember("rarity") || !jsonLoot["rarity"].IsInt()) return;

    if (!jsonLoot.HasMember("group")) return;
    const auto& jsonGroup = jsonLoot["group"];
    if (!jsonGroup.IsString()) return;

    const auto& itGroup = g_Data.distGroups.find(jsonGroup.GetString());
    if (itGroup == g_Data.distGroups.end()) return;

    auto profile = &itProfile->second;
    auto group = &itGroup->second;
    int rarity = std::clamp(jsonLoot["rarity"].GetInt(), 0, 2);

    RE::TESBoundObject* piece = nullptr;
    ArmorSet items;

    if (jsonLoot.HasMember("piece") && jsonLoot["piece"].GetBool()) piece = item;

    if (auto armor = item->As<RE::TESObjectARMO>()) {
        if (DoNotDistribute(armor)) {
            piece = nullptr;
        } else if (jsonLoot.HasMember("set")) {
            for (const auto& i : jsonLoot["set"].GetArray()) {
                RE::FormID id = GetFullId(item->GetFile(), i.GetUint());

                if (auto setitem = RE::TESForm::LookupByID<RE::TESObjectARMO>(id)) {
                    if (!DoNotDistribute(setitem))
                        items.push_back(setitem);
                }
            }
        }
    }

    if (piece || !items.empty())
        g_Data.loot->mapItemDist[item] = {profile, group, rarity, piece, std::move(items)};
}

void LoadContainerList(std::map<RE::TESForm*, QuickArmorRebalance::ContainerChance>& set, const Value& jsonList) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!jsonList.IsObject()) return;
    for (const auto& jsonFiles : jsonList.GetObj()) {
        if (auto file = dataHandler->LookupModByName(jsonFiles.name.GetString())) {
            if (!jsonFiles.value.IsObject()) continue;
            for (const auto& entry : jsonFiles.value.GetObj()) {
                if (auto form = QuickArmorRebalance::FindIn<RE::TESForm>(file, entry.name.GetString())) {
                    if (form->As<RE::TESContainer>() || form->As<RE::TESLevItem>()) {
                        if (entry.value.IsObject()) {
                            auto num = GetJsonInt(entry.value, "num", 1, 20);
                            auto chance = GetJsonInt(entry.value, "chance", 1, 100, 100);
                            set[form] = {num, chance};
                        } else if (entry.value.IsInt()) {
                            set[form] = {1, std::clamp(entry.value.GetInt(), 1, 100)};
                        } else
                            set[form] = {1, 100};

                    } else
                        logger::warn("Item not container or leveled list: {}", entry.name.GetString());
                } else
                    logger::warn("Item not found: {}", entry.name.GetString());
            }
        }
    }
}

void QuickArmorRebalance::LoadLootConfig(const Value& jsonLoot) {
    if (jsonLoot.HasMember("containerGroups")) {
        const auto& jsonGroups = jsonLoot["containerGroups"];
        if (jsonGroups.IsObject()) {
            for (const auto& jsonGroup : jsonGroups.GetObj()) {
                auto& group = g_Data.loot->containerGroups[jsonGroup.name.GetString()];
                if (jsonGroup.value.HasMember("sets")) LoadContainerList(group.large, jsonGroup.value["sets"]);
                if (jsonGroup.value.HasMember("pieces")) LoadContainerList(group.small, jsonGroup.value["pieces"]);
                if (jsonGroup.value.HasMember("weapons")) LoadContainerList(group.weapon, jsonGroup.value["weapons"]);
            }
        }
    }

    if (jsonLoot.HasMember("distGroups")) {
        const auto& jsonGroups = jsonLoot["distGroups"];
        if (jsonGroups.IsObject()) {
            for (const auto& jsonGroup : jsonGroups.GetObj()) {
                auto& group = g_Data.distGroups[jsonGroup.name.GetString()];

                group.name = jsonGroup.name.GetString();
                group.level = GetJsonInt(jsonGroup.value, "level", 1, 255);
                group.early = GetJsonInt(jsonGroup.value, "early", 0, 50);
                group.peak = GetJsonInt(jsonGroup.value, "peak", 1, 50);
                group.falloff = GetJsonInt(jsonGroup.value, "falloff", 0, 50);
                group.minw = GetJsonInt(jsonGroup.value, "minw", 0, 20);
                group.maxw = GetJsonInt(jsonGroup.value, "maxw", 1, 20);
            }
        }
    }

    if (jsonLoot.HasMember("distProfiles")) {
        const auto& jsonGroups = jsonLoot["distProfiles"];
        if (jsonGroups.IsObject()) {
            for (const auto& jsonGroup : jsonGroups.GetObj()) {
                g_Config.lootProfiles.insert(jsonGroup.name.GetString());

                auto& group = g_Data.loot->distProfiles[jsonGroup.name.GetString()];

                if (jsonGroup.value.HasMember("in")) {
                    const auto& jsonIn = jsonGroup.value["in"];
                    if (!jsonIn.IsArray()) continue;
                    for (const auto& name : jsonIn.GetArray()) {
                        if (!name.IsString()) continue;
                        group.containerGroups.insert(&g_Data.loot->containerGroups[name.GetString()]);
                    }
                }
            }
        }
    }
}

void QuickArmorRebalance::ValidateLootConfig() {
    for (const auto& i : g_Data.loot->containerGroups) {
        if (i.second.small.empty() && i.second.large.empty())
            logger::warn("Loot container group {} has no associated containers", i.first);
    }

    for (const auto& i : g_Data.distGroups) {
        if (i.second.level < 0)
            logger::warn("Loot distribution group {} is referenced by an armor set but does not exist (case sensitive)",
                         i.first);
    }

    for (const auto& i : g_Data.loot->distProfiles) {
        if (i.second.containerGroups.empty())
            logger::warn("Loot profile group {} has no associated container groups", i.first);
    }
}

namespace {
    using namespace QuickArmorRebalance;

    int g_nLListsCreated = 0;

    const size_t kLLMaxSize = 0xff;  // Not 0x100 because num_entries would roll to 0 at max size

    RE::TESLevItem* CreateLeveledList() {
        auto dataHandler = RE::TESDataHandler::GetSingleton();

        auto newForm =
            static_cast<RE::TESLevItem*>(RE::IFormFactory::GetFormFactoryByType(RE::FormType::LeveledItem)->Create());

        if (newForm) {
            dataHandler->GetFormArray<RE::TESLevItem>().push_back(newForm);
        }

        g_nLListsCreated++;
        return newForm;
    }

    void LogListContents(RE::TESLevItem* list) { 
        logger::info(">>>List: Size {}, Chance Zero {}%", list->numEntries, list->chanceNone);

        for (int i = 0; i < list->numEntries; i++)
        {
            if (!list->entries[i].form)
                logger::error("{}: [{}] NULL", i, list->entries[i].level);
            else if (auto sublist = list->entries[i].form->As<RE::TESLevItem>())
                logger::info("{}: [{}] Sublist size {}, chance zero {}% ({})", i, list->entries[i].level,
                             sublist->numEntries, sublist->chanceNone,
                             list->entries[i].count);
            else
                logger::info("{}: [{}] {} ({})", i, list->entries[i].level, list->entries[i].form->GetName(),
                             list->entries[i].count);
        }

        logger::info(">>>End list<<<");
    }

    template <class T>
    RE::TESBoundObject* BuildListFrom(T* const* items, size_t count, uint8_t flags, uint8_t chanceNone = 0) {
        if (!count) return nullptr;
        if (count == 1 && !chanceNone) return const_cast<T*>(*items);
        if (count <= kLLMaxSize) {
            auto list = CreateLeveledList();
            list->llFlags = (RE::TESLeveledList::Flag)flags;
            list->entries.resize(list->numEntries = (uint8_t)count);
            list->chanceNone = chanceNone;
            for (int i = 0; i < count; i++) {
                auto& e = list->entries[i];
                e.count = 1;
                e.form = items[i];
                e.level = 1;
                e.itemExtra = nullptr;
            }
            //LogListContents(list);
            return list;
        } else {
            auto split = 1 + count / kLLMaxSize;
            auto slice = count / split;

            auto list = CreateLeveledList();
            list->llFlags = (RE::TESLeveledList::Flag)flags;
            list->entries.resize(split);
            list->chanceNone = chanceNone;
            auto front = items;
            for (int i = 0; i < split; i++) {
                auto& e = list->entries[i];
                e.count = 1;
                e.form = BuildListFrom(front, count > kLLMaxSize ? slice : count, flags);
                e.level = 1;
                e.itemExtra = nullptr;

                front += slice;
                count -= slice;
            }

            //LogListContents(list);
            return list;
        }
    }

    RE::TESBoundObject* BuildListFrom(const std::vector<RE::TESObjectARMO*>& items, uint8_t flags) {
        return BuildListFrom(items.data(), items.size(), flags);
    }

    RE::TESBoundObject* BuildListFrom(const std::vector<RE::TESBoundObject*>& items, uint8_t flags) {
        return BuildListFrom(items.data(), items.size(), flags);
    }

    RE::TESBoundObject* BuildContentList(const std::vector<RE::TESBoundObject*>& contents) {
        if (!QuickArmorRebalance::g_Config.bNormalizeModDrops)
            return BuildListFrom(contents, RE::TESLeveledList::kCalculateForEachItemInCount);

        std::vector<RE::TESBoundObject*> modLists;
        std::map<RE::TESFile*, std::vector<RE::TESBoundObject*>> map;

        for (auto i : contents) map[i->GetFile(0)].push_back(i);
        for (const auto& i : map)
        {
            modLists.push_back(BuildListFrom(i.second, RE::TESLeveledList::kCalculateForEachItemInCount));
        }

        return BuildListFrom(modLists, RE::TESLeveledList::kCalculateForEachItemInCount);
    }

    RE::TESBoundObject* BuildArmorSetList(const QuickArmorRebalance::ArmorSet* set) {
        auto it = QuickArmorRebalance::g_Data.loot->setList.find(set);
        if (it != QuickArmorRebalance::g_Data.loot->setList.end()) return it->second;

        unsigned int covered = 0;
        std::vector<RE::TESBoundObject*> pieces;

        for (auto i : *set) {
            if (covered & (unsigned int)i->GetSlotMask()) continue;

            unsigned int slots = (unsigned int)i->GetSlotMask();
            std::vector<RE::TESObjectARMO*> conflicts;

            // Two passes - first find potential conflicts, then pick them out
            // This has to happen just to cover weird situations where pieces overlap inconsistently
            // Technically would require repeating until it stops changing, but you'd have to design an armor set just
            // to be obnoxious intentionally
            for (auto j : *set)
                if (slots & (unsigned int)j->GetSlotMask()) slots |= (unsigned int)j->GetSlotMask();
            for (auto j : *set)
                if (slots & (unsigned int)j->GetSlotMask()) conflicts.push_back(j);

            if (conflicts.size() > 1)
                pieces.push_back(BuildListFrom(conflicts, 0));
            else
                pieces.push_back(i);

            covered |= slots;
        }

        return QuickArmorRebalance::g_Data.loot->setList[set] = BuildListFrom(pieces, RE::TESLeveledList::kUseAll);
    }

    RE::TESBoundObject* BuildContentList(const std::vector<const QuickArmorRebalance::ArmorSet*>& contents) {
        if (!QuickArmorRebalance::g_Config.bNormalizeModDrops) {
            std::vector<RE::TESBoundObject*> sets;

            for (auto i : contents) sets.push_back(BuildArmorSetList(i));

            return BuildListFrom(sets, RE::TESLeveledList::kCalculateForEachItemInCount);
        }

        std::vector<RE::TESBoundObject*> modLists;
        std::map<RE::TESFile*, std::vector<const QuickArmorRebalance::ArmorSet*>> map;

        for (const auto& i : contents) map[(*i)[0]->GetFile(0)].push_back(i);
        for (const auto& i : map) {
            std::vector<RE::TESBoundObject*> sets;
            for (auto j : i.second)               
                sets.push_back(BuildArmorSetList(j));
            modLists.push_back(BuildListFrom(sets, RE::TESLeveledList::kCalculateForEachItemInCount));
        }

        return BuildListFrom(modLists, RE::TESLeveledList::kCalculateForEachItemInCount);

    }

    template <class T>
    RE::TESBoundObject* BuildGroupList(const T* contents) {
        RE::TESBoundObject* lists[3] = {nullptr, nullptr, nullptr};

        int nUsed = 0;
        for (int i = 0; i < 3; i++) {
            if ((lists[i] = BuildContentList(contents[i]))) nUsed++;
        }

        constexpr int weight[] = {15, 4, 1};
        constexpr int weightTotal = 20;

        if (!nUsed) return nullptr;
        if (nUsed == 1) {
            for (int i = 0; i < 3; i++)
                if (lists[i]) {
                    if (!g_Config.bEnableRarityNullLoot)
                        return lists[i];
                    else
                        return BuildListFrom(&lists[i], 1, RE::TESLeveledList::kCalculateForEachItemInCount,
                                             100 - (uint8_t)(100.f * (float)weight[i] / weightTotal));
                }
        }

        std::vector<RE::TESBoundObject*> r;

        for (int i = 0; i < 3; i++) {
            if (lists[i] || g_Config.bEnableRarityNullLoot)
                for (int n = 0; n < weight[i]; n++) r.push_back(lists[i]);
        }

        return BuildListFrom(r, RE::TESLeveledList::kCalculateForEachItemInCount);
    }

    int GetGroupEntriesForLevel(int level, QuickArmorRebalance::LootDistGroup* group) {
        auto r = group->level - level;
        if (r > group->early) return 0;
        if (r > 0) return (int)std::round(std::lerp(group->maxw, 1, (float)r / group->early));
        r += group->peak;
        if (r >= 0) return group->maxw;

        r += group->falloff;
        if (r > 0) return (int)std::round(std::lerp(group->minw, group->maxw, (float)r / group->falloff));

        return group->minw;
    }

    RE::TESForm* BuildCurve(int level,
                            const std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*>& lists) {
        if (lists.empty()) return nullptr;
        if (lists.size() == 1) return lists.begin()->second;

        std::vector<RE::TESBoundObject*> entries;

        for (auto& i : lists) {
            auto n = GetGroupEntriesForLevel(level, i.first);
            // logger::info("{}[{}]: {}", i.first->name, level, n);
            while (n-- > 0) entries.push_back(i.second);
        }

        return BuildListFrom(entries, 0);
    }

    RE::TESLevItem* BuildCurveList(std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> lists) {
        if (lists.empty()) return nullptr;

        std::vector<std::pair<uint16_t, RE::TESForm*>> curves;

        int minLevel = 0xffff;
        int maxLevel = 0;

        for (auto& i : lists) {
            minLevel = std::min(minLevel, i.first->level - i.first->early);
            maxLevel = std::max(maxLevel, i.first->level + i.first->peak);
        }

        minLevel = std::max(minLevel, 1);
        maxLevel = std::min(maxLevel, 255);

        for (int level = minLevel; level <= maxLevel;
             level = level < maxLevel ? std::min(level + QuickArmorRebalance::g_Config.levelGranularity, maxLevel) : maxLevel + 1) {
            auto curve = BuildCurve(level, lists);

            if (curve) curves.push_back({level, curve});
        }

        auto list = CreateLeveledList();
        list->llFlags = RE::TESLeveledList::kCalculateForEachItemInCount;
        list->entries.resize(list->numEntries = (uint8_t)curves.size());
        for (int i = 0; i < list->numEntries; i++) {
            auto& e = list->entries[i];
            e.count = 1;
            e.form = curves[i].second;
            e.level = curves[i].first;
            e.itemExtra = nullptr;
        }

        return list;
    }

    void FillContents(const std::map<RE::TESForm*, QuickArmorRebalance::ContainerChance>& containers,
                      RE::TESLevItem* curveList) {
        std::map<int, RE::TESBoundObject*> chances;

        for (auto& entry : containers) {
            if (entry.second.chance <= 0) continue;

            auto list = chances[entry.second.chance];
            if (!list) {
                auto chance = std::clamp((int)std::round(entry.second.chance * QuickArmorRebalance::g_Config.fDropRates / 100.0f), 1, 100);
                if (chance < 100) {
                    chances[entry.second.chance] = list =
                        BuildListFrom(&curveList, 1, RE::TESLeveledList::kCalculateForEachItemInCount,
                                      (int8_t)(100 - entry.second.chance));
                } else {
                    chances[entry.second.chance] = list = curveList;  // No reason to make an intermediate table at 100%
                }
            }

            if (auto container = entry.first->As<RE::TESContainer>()) {
                container->AddObjectToContainer(list, entry.second.count, nullptr);
            } else if (auto llist = entry.first->As<RE::TESLevItem>()) {
                if (llist->numEntries < kLLMaxSize) {
                    llist->entries.resize(llist->entries.size() + 1);

                    auto& e = llist->entries.back();
                    e.count = (uint16_t)entry.second.count;
                    e.form = list;
                    e.level = 1;
                    e.itemExtra = nullptr;

                    llist->numEntries++;
                }
            }
        }
    }

    void BuildSetLists() {
        for (const auto& i : QuickArmorRebalance::g_Data.loot->containerGroups) {
            auto& group = i.second;

            if (!group.small.empty()) {
                std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;
                for (auto& j : group.pieces) {
                    if (auto list = BuildGroupList(j.second)) contentsLists[j.first] = list;
                }

                if (auto pieceList = BuildCurveList(std::move(contentsLists))) {
                    FillContents(group.small, pieceList);
                }
            }

            if (!group.large.empty()) {
                std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;
                for (auto& j : group.sets) {
                    if (auto list = BuildGroupList(j.second)) contentsLists[j.first] = list;
                }

                if (auto pieceList = BuildCurveList(std::move(contentsLists))) {
                    FillContents(group.large, pieceList);
                }
            }

            if (!group.weapon.empty()) {
                std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;
                for (auto& j : group.weapons) {
                    if (auto list = BuildGroupList(j.second)) contentsLists[j.first] = list;
                }

                if (auto pieceList = BuildCurveList(std::move(contentsLists))) {
                    FillContents(group.weapon, pieceList);
                }
            }
        }
    }
}

void QuickArmorRebalance::SetupLootLists() {
    if (g_Config.fDropRates <= 0.0f)
    {
        logger::info("Skipping loot additions because drop rate is 0 or below");
        return;
    }

    logger::info("Processing loot additions");

    for (const auto& i : g_Data.loot->mapItemDist) {
        auto& data = i.second;
        for (auto c : data.profile->containerGroups) {
            if (data.piece) {
                if (auto armor = data.piece->As<RE::TESObjectARMO>()) c->pieces[data.group][data.rarity].push_back(armor);
                else if (auto weap = data.piece->As<RE::TESObjectWEAP>())
                    c->weapons[data.group][data.rarity].push_back(weap);
            }
            if (!data.set.empty()) c->sets[data.group][data.rarity].push_back(&data.set);
        }
    }

    BuildSetLists();
    logger::info("Done processing loot, {} lists created", g_nLListsCreated);
}
