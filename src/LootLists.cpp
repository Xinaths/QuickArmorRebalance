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

// #define RUN_DISTRIBUTION_TESTS 10
// #define TEST_FOR_DUPLICATE_LISTS

using namespace rapidjson;
using namespace QuickArmorRebalance;

namespace {
    bool DoNotDistribute(RE::TESObjectARMO* armor) {
        if (g_Config.bPreventDistributionOfDynamicVariants) {
            for (auto addon : armor->armorAddons) {
                if (g_Data.loot->dynamicVariantsDAV.contains(addon)) return true;
            }
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

Value QuickArmorRebalance::MakeLootChanges(const ArmorChangeParams& params, RE::TESBoundObject* i, MemoryPoolAllocator<>& al) {
    if (!params.bDistribute || !(params.bMerge || (params.armorSet && params.distProfile && params.rarity >= 0))) return {};

    auto& data = *params.data;
    for (auto item : data.items)  // Don't build loot sets from current armor (aka mixed mod sets) - maybe in the future
        if (item->GetFile(0) != i->GetFile(0)) return {};

    if (!data.isWornArmor) {
        Value loot(kObjectType);

        if (auto armor = i->As<RE::TESObjectARMO>()) {
            if (params.bDistAsSet && ((unsigned int)armor->GetSlotMask() & (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kBody) != 0) {
                if (params.bMatchSetPieces) {
                    auto s = BuildSetFrom(i, data.items);
                    if (!s.empty()) {
                        Value setv(kArrayType);
                        for (auto j : s) setv.PushBack(GetFileId(j), al);
                        loot.AddMember("set", setv, al);
                    }
                } else {
                    // If its not matching, we actually can just throw everything into one set and it'll be mixed
                    // properly on loading
                    if (!data.bMixedSetDone) {
                        Value setv(kArrayType);
                        for (auto j : data.items) setv.PushBack(GetFileId(j), al);
                        loot.AddMember("set", setv, al);
                        data.bMixedSetDone = true;
                    }
                }
            }

            if (params.bDistAsPieces) loot.AddMember("piece", true, al);
        } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
            if (params.bDistAsPieces) loot.AddMember("piece", true, al);
        } else if (auto ammo = i->As<RE::TESAmmo>()) {
            if (params.bDistAsPieces) loot.AddMember("piece", true, al);
        }

        if (params.bMerge || !loot.ObjectEmpty()) {
            if (params.distProfile) loot.AddMember("profile", Value(params.distProfile, al), al);
            if (params.armorSet) loot.AddMember("group", Value(params.armorSet->loot->name.c_str(), al), al);
            if (params.rarity >= 0) loot.AddMember("rarity", params.rarity, al);
            if (params.region && params.region != kRegion_KeepPrevious) loot.AddMember("region", Value(params.region->name.c_str(), al), al);

            return loot;
        }
    }

    return Value();
}

void QuickArmorRebalance::LoadLootChanges(RE::TESBoundObject* item, const Value& jsonLoot, unsigned int& changed) {
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

    changed |= eChange_Loot;

    const Region* region = nullptr;
    if (g_Config.bEnableRegionalLoot && jsonLoot.HasMember("region")) {
        changed |= eChange_Region;
        const auto& jsonRegion = jsonLoot["region"];

        if (jsonRegion.IsString()) {
            auto nameHash = std::hash<std::string>{}(MakeLower(jsonRegion.GetString()));
            if ((region = MapFind(g_Config.mapRegions, nameHash))) {
                if (!region->IsValid()) region = nullptr;
            }
        }
    }

    auto profile = &itProfile->second;
    auto group = &itGroup->second;
    int rarity = std::clamp(jsonLoot["rarity"].GetInt(), 0, 2);

    RE::TESBoundObject* piece = nullptr;
    ArmorSet items;

    if (jsonLoot.HasMember("piece") && jsonLoot["piece"].GetBool()) {
        piece = item;
        g_Data.distItems.insert(item);
    }

    if (auto armor = item->As<RE::TESObjectARMO>()) {
        if (DoNotDistribute(armor)) {
            piece = nullptr;
        } else if (jsonLoot.HasMember("set")) {
            for (const auto& i : jsonLoot["set"].GetArray()) {
                RE::FormID id = GetFullId(item->GetFile(), i.GetUint());

                if (auto setitem = RE::TESForm::LookupByID<RE::TESObjectARMO>(id)) {
                    if (!DoNotDistribute(setitem)) {
                        items.push_back(setitem);
                        g_Data.distItems.insert(setitem);
                    }
                }
            }
        }
    }

    if (piece || !items.empty()) g_Data.loot->mapItemDist[item] = {profile, group, region, rarity, piece, std::move(items)};
}

void LoadContainerList(const RE::TESFile* file, std::map<RE::TESForm*, QuickArmorRebalance::ContainerChance>& set, const Value& jsonList) {
    if (!jsonList.IsObject()) return;
    for (const auto& entry : jsonList.GetObj()) {
        if (auto form = QuickArmorRebalance::FindIn<RE::TESForm>(file, entry.name.GetString())) {
            auto container = form->As<RE::TESContainer>();
            if (container || form->As<RE::TESLevItem>()) {
                ContainerChance params;
                if (entry.value.IsObject()) {
                    params.count = GetJsonInt(entry.value, "num", 1, 20);
                    params.chance = GetJsonInt(entry.value, "chance", 1, 100, 100);
                    params.ench.enchPower = GetJsonFloat(entry.value, "enchPower", 0.5f, 3.0f, 1.0f);
                    params.ench.enchRate = GetJsonFloat(entry.value, "enchRate", 0.0f, 5.0f, 1.0f);
                } else if (entry.value.IsInt()) {
                    params.chance = std::clamp(entry.value.GetInt(), 1, 100);
                }

                set[form] = params;

                if (container) g_Data.loot->mapContainerCopy[form] = {};
            } else
                logger::warn("Item not container or leveled list: {}", entry.name.GetString());
        } else
            logger::warn("Item not found: {}", entry.name.GetString());
    }
}

void LoadContainerRegions(LootContainerGroup& group, const Value& jsonList) {
    if (!jsonList.IsObject()) return;

    auto dataHandler = RE::TESDataHandler::GetSingleton();

    for (const auto& jsonRegion : jsonList.GetObj()) {
        if (!jsonRegion.value.IsObject()) continue;
        auto region = g_Config.GetRegion(jsonRegion.name.GetString());

        for (const auto& jsonFiles : jsonRegion.value.GetObj()) {
            if (!jsonFiles.value.IsObject()) continue;
            if (auto file = dataHandler->LookupModByName(jsonFiles.name.GetString())) {
                auto& value = jsonFiles.value;
                if (value.HasMember("sets")) LoadContainerList(file, group.large[region], value["sets"]);
                if (value.HasMember("pieces")) LoadContainerList(file, group.small[region], value["pieces"]);
                if (value.HasMember("weapons")) LoadContainerList(file, group.weapon[region], value["weapons"]);
            }
        }
    }
}

void LoadMigrateRarityList(LootContainerGroup& group, const Value& node, int rarity, bool bFrom) {
    if (!node.IsArray()) return;

    for (auto& jsonOther : node.GetArray()) {
        if (!jsonOther.IsString()) continue;

        auto& other = g_Data.loot->containerGroups[jsonOther.GetString()];
        if (&other == &group) continue;

        if (bFrom) {
            group.migration.Add(&other, rarity);
        } else {
            other.migration.Add(&group, rarity);
        }
    }
}

void LoadMigrateList(LootContainerGroup& group, const Value& node, const char* name, bool bFrom) {
    if (!node.HasMember(name)) return;

    auto& rarities = node[name];
    if (!rarities.IsObject()) return;

    static const char* rarityNames[] = {"same", "common", "uncommon", "rare", "exotic", "none"};
    for (int i = 0; i <= eRegion_RarityCount; i++) {
        if (rarities.HasMember(rarityNames[i])) LoadMigrateRarityList(group, rarities[rarityNames[i]], i, bFrom);
    }
}

void QuickArmorRebalance::LoadLootConfig(const Value& jsonLoot) {
    if (jsonLoot.HasMember("containerGroups")) {
        const auto& jsonGroups = jsonLoot["containerGroups"];
        if (jsonGroups.IsObject()) {
            for (const auto& jsonGroup : jsonGroups.GetObj()) {
                auto& group = g_Data.loot->containerGroups[jsonGroup.name.GetString()];
                if (group.name.empty()) group.name = jsonGroup.name.GetString();

                if (jsonGroup.value.HasMember("leveled")) group.bLeveled = jsonGroup.value["leveled"].GetBool();
                if (jsonGroup.value.HasMember("enchPower")) group.ench.enchPower = jsonGroup.value["enchPower"].GetFloat();
                if (jsonGroup.value.HasMember("enchRate")) group.ench.enchRate = jsonGroup.value["enchRate"].GetFloat();

                if (jsonGroup.value.HasMember("containers")) LoadContainerRegions(group, jsonGroup.value["containers"]);
                if (jsonGroup.value.HasMember("regions")) {
                    auto& jsonRegions = jsonGroup.value["regions"];
                    if (jsonRegions.IsArray()) {
                        for (auto& name : jsonRegions.GetArray()) {
                            if (name.IsString()) group.regions.insert(g_Config.GetRegion(name.GetString()));
                        }
                    }
                }
                if (jsonGroup.value.HasMember("migrate")) {
                    auto& jsonMigrate = jsonGroup.value["migrate"];
                    if (jsonMigrate.IsObject()) {
                        LoadMigrateList(group, jsonMigrate, "from", true);
                        LoadMigrateList(group, jsonMigrate, "to", false);
                    }
                }
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

                g_Config.levelMaxDist = std::max(g_Config.levelMaxDist, group.level);
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
    logger::trace("Validating loot configuration");

    for (auto& i : g_Data.loot->containerGroups) {
        if (i.second.small.empty() && i.second.large.empty()) logger::warn("Loot container group {} has no associated containers", i.first);
    }

    for (const auto& i : g_Data.distGroups) {
        if (i.second.level < 0) logger::warn("Loot distribution group {} is referenced by an armor set but does not exist (case sensitive)", i.first);
    }

    for (const auto& i : g_Data.loot->distProfiles) {
        if (i.second.containerGroups.empty()) logger::warn("Loot profile group {} has no associated container groups", i.first);
    }
}

std::map<std::string, RE::TESBoundObject*> g_TestList;

namespace {
    using namespace QuickArmorRebalance;

    int g_nLListsCreated = 0;
    std::map<const char*, int> g_nLLTypes;

#ifdef TEST_FOR_DUPLICATE_LISTS
    std::vector<RE::TESLevItem*> g_createdLists;
#endif

    const size_t kLLMaxSize = 0xff;  // Not 0x100 because num_entries would roll to 0 at max size

    RE::TESLevItem* CreateLeveledList(const char* reason) {
        auto dataHandler = RE::TESDataHandler::GetSingleton();

        auto newForm = static_cast<RE::TESLevItem*>(RE::IFormFactory::GetFormFactoryByType(RE::FormType::LeveledItem)->Create());

        if (newForm) {
            dataHandler->GetFormArray<RE::TESLevItem>().push_back(newForm);
        }

        g_nLListsCreated++;
        g_nLLTypes[reason]++;
        return newForm;
    }

    void LogListContents(RE::TESLevItem* list) {
        logger::info(">>>List: Size {}, Chance Zero {}%", list->numEntries, list->chanceNone);

        for (int i = 0; i < list->numEntries; i++) {
            if (!list->entries[i].form)
                logger::error("{}: [{}] NULL", i, list->entries[i].level);
            else if (auto sublist = list->entries[i].form->As<RE::TESLevItem>())
                logger::info("{}: [{}] Sublist size {}, chance zero {}% ({})", i, list->entries[i].level, sublist->numEntries, sublist->chanceNone, list->entries[i].count);
            else
                logger::info("{}: [{}] {} ({})", i, list->entries[i].level, list->entries[i].form->GetName(), list->entries[i].count);
        }

        logger::info(">>>End list<<<");
    }

    void AddListEntries(std::vector<RE::TESBoundObject*>& entries, RE::TESBoundObject* entry, int n) {
        while (n--) entries.push_back(entry);
    }

    template <class T>
    RE::TESBoundObject* BuildListFrom(const char* purpose, T* const* items, size_t count, uint8_t flags, uint8_t chanceNone = 0) {
        if (!count) return nullptr;
        if (!chanceNone) {
            if (count == 1) return const_cast<T*>(*items);

            // Might be multiple entries of the same item
            if (items[0] == items[count - 1]) {  // First and last is quick check, then verify
                for (int i = 1; i < count; i++) {
                    if (items[0] != items[i]) goto NotSame;
                }
                return const_cast<T*>(*items);
            }
        }
    NotSame:
        if (count <= kLLMaxSize) {
            auto list = CreateLeveledList(purpose);
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
            // LogListContents(list);
            return list;
        } else {
            auto split = 1 + count / kLLMaxSize;
            auto slice = count / split;

            auto list = CreateLeveledList(purpose);
            list->llFlags = (RE::TESLeveledList::Flag)flags;
            list->entries.resize(split);
            list->chanceNone = chanceNone;
            auto front = items;
            for (int i = 0; i < split; i++) {
                auto& e = list->entries[i];
                e.count = 1;
                e.form = BuildListFrom(purpose, front, count > kLLMaxSize ? slice : count, flags);
                e.level = 1;
                e.itemExtra = nullptr;

                front += slice;
                count -= slice;
            }

            // LogListContents(list);
#ifdef TEST_FOR_DUPLICATE_LISTS
            g_createdLists.push_back(list);
#endif
            return list;
        }
    }

    RE::TESBoundObject* BuildListFrom(const char* purpose, const std::vector<RE::TESObjectARMO*>& items, uint8_t flags) {
        return BuildListFrom(purpose, items.data(), items.size(), flags);
    }

    RE::TESBoundObject* BuildListFrom(const char* purpose, const std::vector<RE::TESBoundObject*>& items, uint8_t flags) {
        return BuildListFrom(purpose, items.data(), items.size(), flags);
    }

    RE::TESBoundObject* BuildContentList(const std::vector<RE::TESBoundObject*>& contents) {
        if (!QuickArmorRebalance::g_Config.bNormalizeModDrops) return BuildListFrom("Items", contents, RE::TESLeveledList::kCalculateForEachItemInCount);

        std::vector<RE::TESBoundObject*> modLists;
        std::map<RE::TESFile*, std::vector<RE::TESBoundObject*>> map;

        for (auto i : contents) map[i->GetFile(0)].push_back(i);
        for (const auto& i : map) {
            modLists.push_back(BuildListFrom("Items", i.second, RE::TESLeveledList::kCalculateForEachItemInCount));
        }

        return BuildListFrom("Mod Grouped Items", modLists, RE::TESLeveledList::kCalculateForEachItemInCount);
    }

    RE::TESBoundObject* BuildArmorSetList(const QuickArmorRebalance::ArmorSet* set) {
        auto it = QuickArmorRebalance::g_Data.loot->setList.find(set);
        if (it != QuickArmorRebalance::g_Data.loot->setList.end()) return it->second;

        unsigned int covered = 0;
        std::vector<RE::TESBoundObject*> pieces;

        for (auto i : *set) {
            auto slots = (ArmorSlots)i->GetSlotMask();
            if (!slots) continue;
            if (covered & slots) continue;

            std::vector<RE::TESObjectARMO*> conflicts;

            // Two passes - first find potential conflicts, then pick them out
            // This has to happen just to cover weird situations where pieces overlap inconsistently
            // Technically would require repeating until it stops changing, but you'd have to design an armor set just
            // to be obnoxious intentionally
            for (auto j : *set) {
                auto slots2 = (ArmorSlots)j->GetSlotMask();
                if (slots & slots2) slots |= slots2;
            }
            for (auto j : *set) {
                auto slots2 = (ArmorSlots)j->GetSlotMask();
                if (slots & slots2) conflicts.push_back(j);
            }

            if (conflicts.size() > 1)
                pieces.push_back(BuildListFrom("Set Slot Randomization", conflicts, 0));
            else
                pieces.push_back(i);

            covered |= slots;
        }

        return QuickArmorRebalance::g_Data.loot->setList[set] = BuildListFrom("Armor Set", pieces, RE::TESLeveledList::kUseAll);
    }

    RE::TESBoundObject* BuildContentList(const std::vector<const QuickArmorRebalance::ArmorSet*>& contents) {
        if (!QuickArmorRebalance::g_Config.bNormalizeModDrops) {
            std::vector<RE::TESBoundObject*> sets;

            for (auto i : contents) sets.push_back(BuildArmorSetList(i));

            return BuildListFrom("Armor Sets List", sets, RE::TESLeveledList::kCalculateForEachItemInCount);
        }

        std::vector<RE::TESBoundObject*> modLists;
        std::map<RE::TESFile*, std::vector<const QuickArmorRebalance::ArmorSet*>> map;

        for (const auto& i : contents) map[(*i)[0]->GetFile(0)].push_back(i);
        for (const auto& i : map) {
            std::vector<RE::TESBoundObject*> sets;
            for (auto j : i.second) sets.push_back(BuildArmorSetList(j));
            modLists.push_back(BuildListFrom("Armor Sets List", sets, RE::TESLeveledList::kCalculateForEachItemInCount));
        }

        return BuildListFrom("Mod Groupped Armor Sets List", modLists, RE::TESLeveledList::kCalculateForEachItemInCount);
    }

    RE::TESBoundObject* BuildGroupList(const LootContainerGroup::Rarities& contents, const LootContainerGroup::Rarities& fallback, RE::TESBoundObject** lowerTier, auto Fetch) {
        RE::TESBoundObject* ret = nullptr;
        auto cachePtr = &Fetch(contents, 0);
        if (MapFind(g_Data.loot->cacheGroupList, cachePtr, ret)) {
            // Cache unused due to RegionalGroupTierList cache
            // logger::info("Group List cache used");
            return ret;
        }

        RE::TESBoundObject* lists[3] = {nullptr, nullptr, nullptr};

        int nUsed = 0;
        for (int i = 0; i < 3; i++) {
            if ((lists[i] = BuildContentList(!Fetch(contents, i).empty() ? Fetch(contents, i) : Fetch(fallback, i)))) nUsed++;
        }

        constexpr int weight[] = {15, 4, 1};
        constexpr int weightTotal = 20;

        if (!nUsed)
            ret = nullptr;
        else {
            for (int i = 0; i < 3; i++) {
                if (lists[i]) lowerTier[i] = lists[i];
            }

            if (nUsed == 1) {
                if (lists[0])
                    ret = lists[0];  // If its a common, just always return the common
                else {
                    for (int i = 0; i < 3; i++) {
                        if (!lists[i]) {
                            lists[i] = lowerTier[i];
                            nUsed++;
                        }
                    }

                    if (nUsed == 1) {
                        for (int i = 1; i < 3; i++)
                            if (lists[i]) {
                                if (!g_Config.bEnableRarityNullLoot || i == 0)
                                    ret = lists[i];
                                else
                                    ret = BuildListFrom("Rarity List", &lists[i], 1, RE::TESLeveledList::kCalculateForEachItemInCount,
                                                        100 - (uint8_t)(100.f * (float)weight[i] / weightTotal));
                                break;
                            }
                    }
                }
            }

            if (!ret) {
                std::vector<RE::TESBoundObject*> r;

                // Add most rare to least rare, letting it not add more rare entries if they don't exist

                bool bAdd = false;
                for (int i = 2; i >= 0; i--) {
                    if (lists[i]) {
                        bAdd = true;
                    }

                    if (bAdd && !(!lists[i] && !g_Config.bEnableRarityNullLoot)) AddListEntries(r, lowerTier[i], weight[i]);  // Lower tier will have current tier if appropriate
                }

                ret = BuildListFrom("Rarity List", r, RE::TESLeveledList::kCalculateForEachItemInCount);
            }
        }

        return g_Data.loot->cacheGroupList[cachePtr] = ret;
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

    RE::TESForm* BuildCurve(int level, const std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*>& lists) {
        if (lists.empty()) return nullptr;
        if (lists.size() == 1) return lists.begin()->second;

        std::vector<RE::TESBoundObject*> entries;

        for (auto& i : lists) {
            auto n = GetGroupEntriesForLevel(level, i.first);
            // logger::info("{}[{}]: {}", i.first->name, level, n);
            while (n-- > 0) entries.push_back(i.second);
        }

        return BuildListFrom("Level Curve", entries, 0);
    }

    RE::TESBoundObject* BuildCurveList(std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> lists) {
        if (lists.empty()) return nullptr;
        if (lists.size() == 1) {
            if (lists.begin()->first == nullptr)  // Only want to return the list directly if its unleveled - otherwise, we need a list just to have a min level
                return lists.begin()->second;
        }

        assert(!lists.contains(nullptr));

        std::vector<std::pair<uint16_t, RE::TESForm*>> curves;

        int minLevel = 0xffff;
        int maxLevel = 0;

        for (auto& i : lists) {
            minLevel = std::min(minLevel, i.first->level - i.first->early);
            maxLevel = std::max(maxLevel, i.first->level + i.first->peak);
        }

        minLevel = std::max(minLevel, 1);
        maxLevel = std::min(maxLevel, 255);

        for (int level = minLevel; level <= maxLevel; level = level < maxLevel ? std::min(level + QuickArmorRebalance::g_Config.levelGranularity, maxLevel) : maxLevel + 1) {
            auto curve = BuildCurve(level, lists);

            if (curve) curves.push_back({level, curve});
        }

        auto list = CreateLeveledList("Level Curve Selection");
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

    void FillContents(const std::map<RE::TESForm*, QuickArmorRebalance::ContainerChance>& containers, RE::TESBoundObject* curveList, const EnchantProbability& enchBase) {
        if (!curveList) return;

        std::map<int, RE::TESBoundObject*> chances;

        for (auto& entry : containers) {
            if (entry.second.chance <= 0) continue;

            auto list = chances[entry.second.chance];
            if (!list) {
                auto chance = std::clamp((int)std::round(entry.second.chance * QuickArmorRebalance::g_Config.fDropRates / 100.0f), 1, 100);
                if (chance < 100) {
                    chances[entry.second.chance] = list =
                        BuildListFrom("Chance", &curveList, 1, RE::TESLeveledList::kCalculateForEachItemInCount, (int8_t)(100 - entry.second.chance));
                } else {
                    chances[entry.second.chance] = list = curveList;  // No reason to make an intermediate table at 100%
                }
            }

            if (auto container = entry.first->As<RE::TESContainer>()) {
                container->AddObjectToContainer(list, entry.second.count, nullptr);

                // Adding
                auto& ench = g_Data.distContainers[container];

                // Record the enchantment rates
                // This is a mess because we need to store defaults, but we can also have multiple entries
                // Only proper way to ensure a specific enchantment setup is having all container references have the same enchantment rates
                if (!entry.second.ench.IsDefault()) {
                    if (ench.IsDefault() || ench == enchBase) {
                        ench = entry.second.ench;
                        ench.enchPower *= enchBase.enchPower;
                        ench.enchRate *= enchBase.enchRate;
                    } else {  // potentially conflicting entries, just take the more favorable of the two
                        ench.enchRate = std::max(ench.enchRate, entry.second.ench.enchRate * enchBase.enchRate);
                        ench.enchPower = std::max(ench.enchPower, entry.second.ench.enchPower * enchBase.enchPower);
                    }
                } else if (!enchBase.IsDefault()) {
                    if (ench.IsDefault()) ench = enchBase;
                    // Else it is either enchBase, or it's been modified by something else - just leave it alone
                }

                for (auto copyTo : g_Data.loot->mapContainerCopy[entry.first]) {
                    if (auto containerCopy = copyTo->As<RE::TESContainer>()) {
                        containerCopy->AddObjectToContainer(list, entry.second.count, nullptr);
                        g_Data.distContainers[containerCopy] = ench;
                    }
                }
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

    /*
void BuildSetLists() {
for (const auto& i : QuickArmorRebalance::g_Data.loot->containerGroups) {
    auto& group = i.second;
    if (!group.small.empty()) {
        std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;
        for (auto& j : group.pieces) {
            if (auto list = BuildGroupList(j.second)) contentsLists[j.first] = list;
        }

        if (auto pieceList = BuildCurveList(std::move(contentsLists))) {
            FillContents(group.small, pieceList, group.ench);
        }
    }

    if (!group.large.empty()) {
        std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;
        for (auto& j : group.sets) {
            if (auto list = BuildGroupList(j.second)) contentsLists[j.first] = list;
        }

        if (auto pieceList = BuildCurveList(std::move(contentsLists))) {
            FillContents(group.large, pieceList, group.ench);
        }
    }

    if (!group.weapon.empty()) {
        std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;
        for (auto& j : group.weapons) {
            if (auto list = BuildGroupList(j.second)) contentsLists[j.first] = list;
        }

        if (auto pieceList = BuildCurveList(std::move(contentsLists))) {
            FillContents(group.weapon, pieceList, group.ench);
        }
    }
}
}
    */

    /*
    RE::TESBoundObject* BuildRegionalGroupCurveList(ELootType lootType, LootContainerGroup* group, Region* region) {
        RE::TESBoundObject* ret = nullptr;
        if (!group->contents.contains(region)) region = nullptr;

        if (MapFind(g_Data.loot->tblRegionSourceCurveCache[lootType][region], group, ret)) return ret;

        std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;

        for (auto& tier : group->contents[region]) {
            auto& fallback = group->contents[nullptr][tier.first];

            // auto Builder = [](const LootContainerGroup::Rarities& contents, const LootContainerGroup::Rarities& fallback, int rarity) {
            //     return BuildContentList(!contents[rarity].weapons.empty() ? contents[rarity].weapons : fallback[rarity].weapons);
            // };

            RE::TESBoundObject* list = nullptr;

            switch (lootType) {
                case eLoot_Set: {
                    list = BuildGroupList(tier.second, fallback, [](const LootContainerGroup::Rarities& items, int rarity) -> auto& { return items[rarity].sets; });
                } break;
                case eLoot_Armor: {
                    list = BuildGroupList(tier.second, fallback, [](const LootContainerGroup::Rarities& items, int rarity) -> auto& { return items[rarity].pieces; });
                } break;
                case eLoot_Weapon: {
                    list = BuildGroupList(tier.second, fallback, [](const LootContainerGroup::Rarities& items, int rarity) -> auto& { return items[rarity].weapons; });
                } break;
            }

            if (list) contentsLists[tier.first] = list;
        }

        return g_Data.loot->tblRegionSourceCurveCache[lootType][region][group] = BuildCurveList(contentsLists);
    }
    */

    RE::TESBoundObject* BuildRegionalGroupTierList(ELootType lootType, LootContainerGroup* group, Region* region, LootDistGroup* tier) {
        const LootContainerGroup::Rarities* rarities = nullptr;

        auto& fallback = group->contents[nullptr][tier];

        if (auto regionTiers = MapFind(group->contents, region)) {
            rarities = MapFind(*regionTiers, tier);
        }

        if (!rarities) rarities = &fallback;

        RE::TESBoundObject* ret = nullptr;
        auto cachePtr = rarities;
        if (MapFind(g_Data.loot->cacheRegionalGroupTierList[lootType], cachePtr, ret)) {
            // Cache actively used
            // logger::info("RegionalGroupTierList cache used");
            return ret;
        }

        std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;

        RE::TESBoundObject* list = nullptr;

        switch (lootType) {
            case eLoot_Set: {
                list = BuildGroupList(*rarities, fallback, g_Data.loot->cacheLowerTierItems[lootType][group][region],
                                      [](const LootContainerGroup::Rarities& items, int rarity) -> auto& { return items[rarity].sets; });
            } break;
            case eLoot_Armor: {
                list = BuildGroupList(*rarities, fallback, g_Data.loot->cacheLowerTierItems[lootType][group][region],
                                      [](const LootContainerGroup::Rarities& items, int rarity) -> auto& { return items[rarity].pieces; });
            } break;
            case eLoot_Weapon: {
                list = BuildGroupList(*rarities, fallback, g_Data.loot->cacheLowerTierItems[lootType][group][region],
                                      [](const LootContainerGroup::Rarities& items, int rarity) -> auto& { return items[rarity].weapons; });
            } break;
        }

        return g_Data.loot->cacheRegionalGroupTierList[lootType][cachePtr] = list;
    }

    RE::TESBoundObject* BuildSourceSelectionList(ELootType lootType, LootContainerGroup& group, Region* region, LootDistGroup* tier) {
        RE::TESBoundObject* ret = nullptr;
        // if (MapFind(g_Data.loot->tblRegionSourceCache[lootType][region], &group, ret)) return ret;
        auto& cache = g_Data.loot->cacheSourceSelectionList[lootType][tier][&group];
        auto cachePtr = region;
        if (MapFind(cache, cachePtr, ret)) {
            // logger::info("SourceSelectionList cache used");
            return ret;
        }

        if (!g_Config.bEnableMigratedLoot) {
            return cache[cachePtr] = BuildRegionalGroupTierList(lootType, &group, region, tier);
        }

        std::vector<RE::TESBoundObject*> entries;
        for (int i = 0; i < eRegion_RarityCount; i++) {
            std::vector<RE::TESBoundObject*> groupList;
            for (auto iGroup : group.migration[i]) {
                if ((!iGroup->regions.empty() && !iGroup->regions.contains(region))) continue;

                if (auto list = BuildRegionalGroupTierList(lootType, iGroup, region, tier)) groupList.push_back(list);
            }

            if (groupList.empty()) continue;
            AddListEntries(entries, BuildListFrom("Group Selection", groupList, 0), g_Config.nMigrationRarityEntries[i]);
        }

        // logger::info("Building list for [{}] {} - {} - {}", (int)lootType, group.name, region ? region->name : "<universal>", tier ? tier->name : "<unleveled>");

        return cache[cachePtr] = BuildListFrom("Group Rarity Selection", entries, 0);
    }

    RE::TESBoundObject* BuildRegionSelectionList(ELootType lootType, LootContainerGroup& group, Region* region, LootDistGroup* tier) {
        if (!region) {
            logger::info("FIXME");
            return nullptr;
        }

        if (!g_Config.bEnableRegionalLoot || !g_Config.bEnableCrossRegionLoot) {
            return BuildSourceSelectionList(lootType, group, region, tier);
        }

        std::vector<RE::TESBoundObject*> entries;
        for (int i = 0; i < eRegion_RarityCount; i++) {
            std::vector<RE::TESBoundObject*> regionList;
            for (auto iRegion : region->rarity[i]) {
                // if (!iRegion->IsValid() || (!group.regions.empty() && !group.regions.contains(iRegion))) continue;
                if (!iRegion->IsValid()) {
                    continue;
                }

                if (auto list = BuildSourceSelectionList(lootType, group, iRegion, tier)) regionList.push_back(list);
            }

            if (regionList.empty()) continue;
            AddListEntries(entries, BuildListFrom("Region Selection", regionList, 0), g_Config.nRegionRarityEntries[i]);
        }

        return BuildListFrom("Region Rarity Selection", entries, 0);
    }

    RE::TESBoundObject* BuildRegionalCurveSelectionList(ELootType lootType, LootContainerGroup& group, Region* region) {
        std::map<QuickArmorRebalance::LootDistGroup*, RE::TESBoundObject*> contentsLists;

        if (!group.bLeveled) return BuildRegionSelectionList(lootType, group, region, nullptr);

        for (auto tier : g_Data.distGroupsSorted) {
            // auto tier = &distGroup.second;

            auto list = BuildRegionSelectionList(lootType, group, region, tier);
            if (list) contentsLists[tier] = list;
        }

        return BuildCurveList(contentsLists);
    }

    RE::TESBoundObject* BuildContainerGroupLoot(ELootType lootType, LootContainerGroup& group, LootContainerGroup::ContainerChanceMap& containers, Region* region) {
        RE::TESBoundObject* contentsList = nullptr;

        // contentsList = BuildRegionSelectionList(lootType, group, region);
        contentsList = BuildRegionalCurveSelectionList(lootType, group, region);

        FillContents(containers, contentsList, group.ench);
        return contentsList;
    }

    void BuildContainerLootLists() {
        for (auto& i : QuickArmorRebalance::g_Data.loot->containerGroups) {
            auto& group = i.second;

            if (!group.regions.empty()) {
                // Merge any wrong regions into the generic one

                auto Merge = [&](std::map<Region*, LootContainerGroup::ContainerChanceMap>& containers) {
                    auto& d = containers[nullptr];
                    for (auto& it : containers) {
                        if (it.first && !it.second.empty() && (!it.first->IsValid() || !group.regions.contains(it.first))) {
                            logger::info("Container group '{}' has containers in external region '{}', merging into default group", i.first, it.first->name);
                            d.insert(it.second.begin(), it.second.end());
                            it.second.clear();
                        }
                    }
                };

                Merge(group.large);
                Merge(group.small);
                Merge(group.weapon);
            }

            auto Build = [&](ELootType lootType, const std::string& groupName, std::map<Region*, LootContainerGroup::ContainerChanceMap>& regionContainers) {
                for (auto& it : regionContainers) {
                    if (!it.second.empty()) {
                        auto list = BuildContainerGroupLoot(lootType, group, it.second, it.first);
#if RUN_DISTRIBUTION_TESTS > 0
                        if (list) {
                            auto name = std::format("[{}] {} - {}", (int)lootType, groupName.c_str(), it.first->name.c_str());
                            g_TestList[name] = list;
                        }
#else
                        // Reference params to make warnings go away
                        list;
                        groupName;
#endif
                    }
                }
            };

            Build(eLoot_Set, i.first, group.large);
            Build(eLoot_Armor, i.first, group.small);
            Build(eLoot_Weapon, i.first, group.weapon);
        }
    }
}

void SampleItemDistribution(std::vector<RE::TESBoundObject*>& items, RE::TESForm* form, int level, int count) {
    if (!form) return;
    if (auto list = form->As<RE::TESLevItem>()) {
        if (!list->numEntries) return;
        if (((int)(RNG() % 100)) < list->chanceNone) return;

        if (list->llFlags & RE::TESLeveledList::kUseAll) {
            for (int i = 0; i < list->numEntries; i++) SampleItemDistribution(items, list->entries[i].form, level, list->entries[i].count);
        } else {
            int lower = 0;
            int upper = 0;
            for (upper = 0; upper < list->numEntries && level >= list->entries[upper].level; upper++)
                ;
            if (!upper) return;

            if (!(list->llFlags & RE::TESLeveledList::kCalculateFromAllLevelsLTOrEqPCLevel)) {
                lower = upper - 1;
                const auto same = list->entries[lower].level;
                if (list->entries[0].level == same) {
                    lower = 0;
                } else {
                    do lower--;
                    while (list->entries[lower].level == same);
                    lower++;
                }
            }

            if (list->llFlags & RE::TESLeveledList::kCalculateForEachItemInCount) {
                while (count--) {
                    const int i = lower + (RNG() % (upper - lower));
                    SampleItemDistribution(items, list->entries[i].form, level, list->entries[i].count);
                }
            } else {
                const int i = lower + (RNG() % (upper - lower));
                SampleItemDistribution(items, list->entries[i].form, level, count * list->entries[i].count);
            }
        }
    } else if (auto item = form->As<RE::TESBoundObject>())
        items.push_back(item);
}

void QuickArmorRebalance::SetupLootLists() {
    if (g_Config.fDropRates <= 0.0f) {
        logger::info("Skipping loot additions because drop rate is 0 or below");
        return;
    }

    logger::info("Processing loot additions");

    for (const auto& i : g_Data.loot->mapItemDist) {
        auto& data = i.second;
        for (auto c : data.profile->containerGroups) {
            auto group = c->bLeveled ? data.group : nullptr;
            auto& contents = c->contents[data.region][group][data.rarity];

            if (data.piece) {
                if (auto armor = data.piece->As<RE::TESObjectARMO>())
                    contents.pieces.push_back(armor);
                else if (auto weap = data.piece->As<RE::TESObjectWEAP>())
                    contents.weapons.push_back(weap);
            }
            if (!data.set.empty()) contents.sets.push_back(&data.set);
        }
    }

    // Merge global items into regional item lists
    for (auto& i : QuickArmorRebalance::g_Data.loot->containerGroups) {
        auto& group = i.second;
        auto& defaultRegion = group.contents[nullptr];

        for (auto& regionContents : group.contents) {
            if (!regionContents.first) continue;

            for (auto& tier : regionContents.second) {
                for (int rarity = 0; rarity < 3; rarity++) {
                    auto& items = tier.second[rarity];

                    auto Merge = [&](auto& a, auto& b) { a.insert(a.end(), b.begin(), b.end()); };

                    if (!items.pieces.empty()) Merge(items.pieces, defaultRegion[tier.first][rarity].pieces);
                    if (!items.sets.empty()) Merge(items.sets, defaultRegion[tier.first][rarity].sets);
                    if (!items.weapons.empty()) Merge(items.weapons, defaultRegion[tier.first][rarity].weapons);
                }
            }
        }
    }

#ifdef TEST_FOR_DUPLICATE_LISTS
    auto IdenticalLists = [](RE::TESLevItem* a, RE::TESLevItem* b) -> bool {
        if (a->numEntries != b->numEntries) return false;
        for (int i = 0; i < a->numEntries; i++) {
            if (a->entries[i].form != b->entries[i].form) return false;
        }
        return true;
    };

    int nCopies = 0;
    for (int i = 0; i < g_createdLists.size(); i++) {
        for (int j = i + 1; j < g_createdLists.size(); j++) {
            if (IdenticalLists(g_createdLists[i], g_createdLists[j])) {
                logger::info("Duplicate list found");
                nCopies++;
            }
        }
    }
    logger::info("{} duplicate lists", nCopies);
#endif

    logger::trace("Building loot lists");
    // BuildSetLists();
    BuildContainerLootLists();
    logger::info("Done processing loot, {} lists created", g_nLListsCreated);

    for (auto& i : g_nLLTypes) {
        logger::info("   {} lists for {}", i.second, i.first);
    }

#if RUN_DISTRIBUTION_TESTS > 0

    logger::info("Running Distribution tests");

    for (auto& i : g_TestList) {
        for (int level = 1; level <= 51; level += 5) {
            logger::info("{} - Level {}", i.first, level);
            for (int n = 0; n < RUN_DISTRIBUTION_TESTS; n++) {
                logger::info("   Test {}:", n + 1);
                std::vector<RE::TESBoundObject*> items;
                SampleItemDistribution(items, i.second, level, 1);
                if (items.empty()) {
                    logger::info("      Empty!");
                } else {
                    for (auto item : items) {
                        logger::info("      {}", item->GetName());
                    }
                }
            }
        }
    }

#endif

    g_nLLTypes.clear();
}
