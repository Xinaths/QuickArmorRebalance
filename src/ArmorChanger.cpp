#include "ArmorChanger.h"

#include "Config.h"
#include "Data.h"
#include "LootLists.h"
#include "ModIntegrations.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"

using namespace rapidjson;

const int kFlatWeightStore = 10;

namespace {
    using namespace QuickArmorRebalance;

    int GetTotalWeight(RebalanceCurveNode const* node, ArmorSlots mask) {
        if (!(mask & node->GetSlots())) return 0;

        int w = node->weight;
        for (auto& i : node->children) w += GetTotalWeight(&i, mask);
        return w;
    }

    struct SlotRelativeWeight {
        SlotRelativeWeight* base = nullptr;
        RE::TESObjectARMO* item = nullptr;
        int weightBase = 0;
        int weightUsed = 0;
    };

    int PropogateBaseValues(SlotRelativeWeight* values, SlotRelativeWeight* base, RebalanceCurveNode const* node) {
        auto& v = values[node->slot - 30];

        auto w = node->weight;

        for (auto& i : node->children) w += PropogateBaseValues(values, v.item ? &v : base, &i);

        if (v.item) {
            v.base = &v;
            v.weightBase = w;
            return 0;
        } else {
            v.base = base;
            return w;
        }
    }

    int CalcCoveredValues(SlotRelativeWeight* values, unsigned int coveredSlots, RebalanceCurveNode const* node) {
        auto& v = values[node->slot - 30];
        auto w = node->weight;

        for (auto& i : node->children) w += CalcCoveredValues(values, coveredSlots, &i);

        if (coveredSlots & node->GetSlots()) {
            v.weightUsed = w;
            return 0;
        } else
            return w;
    }

    void CopyConditions(RE::TESCondition& tar, const RE::TESCondition& src, void* replace, void* replaceWith);
    void ClearRecipe(RE::BGSConstructibleObject* tar);
    void ReplaceRecipe(ArmorChangeParams::RecipeOptions& opts, RE::BGSConstructibleObject* tar, const RE::BGSConstructibleObject* src, float w, float fCost);

    ArmorSlots RemapSlots(ArmorSlots slots, const ArmorChangeParams& params) {
        if (slots & params.remapMask) {
            ArmorSlots removed = 0;
            ArmorSlots added = 0;
            for (auto s : params.mapArmorSlots) {
                if (slots & (1 << s.first)) {
                    // slotsRemapped = (slotsRemapped & ~(1 << s.first)) | (s.second < 32 ? (1 << s.second) : 0);
                    removed |= (1 << s.first);
                    if (s.second < 32) added |= (1 << s.second);
                }
            }
            slots = added | (slots & ~removed);
        }
        return slots;
    }

    ArmorSlots PromoteHeadSlots(ArmorSlots slots, ArmorSlots coveredHeadSlots) {
        slots &= ~kCosmeticSlotMask;

        // Promote to head slot if needed
        if (slots & coveredHeadSlots) slots = (slots & ~coveredHeadSlots) | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead;
        return slots;
    }

    std::pair<ArmorSlots, ArmorSlots> CalcCoveredSlots(const auto& items, const ArmorChangeParams& params, bool remap = false) {
        ArmorSlots coveredSlots = 0;
        // Build list of slots that will be used for scaling
        for (auto i : items) {
            if (auto armor = i->As<RE::TESObjectARMO>()) {
                auto slots = (ArmorSlots)armor->GetSlotMask();
                if (remap) slots = MapFindOr(g_Data.modifiedArmorSlots, armor, slots);
                coveredSlots |= slots;
            }
        }

        if (remap) coveredSlots = RemapSlots(coveredSlots, params);

        // Head slot is weird and needs special handling
        ArmorSlots coveredHeadSlots = kHeadSlotMask & coveredSlots;

        if (coveredHeadSlots & (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead)
            coveredHeadSlots = 0;  // If we have a head slot, don't need to do anything
        else                       // Pick the best slot only to promote
        {
            coveredHeadSlots &= (coveredHeadSlots ^ (coveredHeadSlots - 1));  // Isolate lowest bit
        }

        return {PromoteHeadSlots(coveredSlots, coveredHeadSlots), coveredHeadSlots};
    }

    void ProcessBaseArmorSet(const ArmorChangeParams& params, ArmorSlots coveredHeadSlots, const auto& fn) {
        for (auto i : params.armorSet->items) {
            auto slots = PromoteHeadSlots((ArmorSlots)i->GetSlotMask(), coveredHeadSlots);

            // Handling multi-slot items makes things a giant mess, just use the lowest one instead
            // while (slots) {  // hair can cause multiple
            if (slots) {
                unsigned long slot;
                _BitScanForward(&slot, slots);
                slots &= slots - 1;  // Removes lowest bit

                fn((ArmorSlot)slot, i);
            }
        }
    }

    ArmorSlots GetAllCoveredSlots(const RebalanceCurveNode& curve) {
        auto slots = curve.GetSlots();
        for (auto& i : curve.children) {
            slots |= GetAllCoveredSlots(i);
        }
        return slots;
    }

    ArmorSlots GetAllCoveredSlots(const RebalanceCurveNode::Tree& curve, ArmorSlot slot) {
        slot += 30;
        for (auto& i : curve) {
            if (i.slot == slot) return GetAllCoveredSlots(i);
        }
        return 0;
    }

    void AddModification(const char* field, const ArmorChangeParams::SliderPair& pair, rapidjson::Value& changes, MemoryPoolAllocator<>& al, bool bIgnoreDefault = false,
                         int flatStore = 1) {
        if (pair.bModify && !(bIgnoreDefault && pair.IsDefault())) {
            if (!pair.bFlat)
                changes.AddMember(StringRef(field), Value(0.01f * pair.fScale), al);
            else
                changes.AddMember(StringRef(field), Value((int)(flatStore * pair.fScale)), al);
        }
    }

    int AddDynamicVariants(const RE::TESFile* file, const ArmorChangeParams& params, rapidjson::Value& ls, MemoryPoolAllocator<>& al);
    bool AddPreferenceVariants(const RE::TESFile* file, const ArmorChangeParams& params, rapidjson::Value& ls, MemoryPoolAllocator<>& al, int& r);

}

ArmorSlots QuickArmorRebalance::GetConvertableArmorSlots(const ArmorChangeParams& params) {
    params.remapMask = 0;
    for (auto i : params.mapArmorSlots) params.remapMask |= (1 << i.first);

    if (!params.armorSet) return (ArmorSlots)~0;

    auto [coveredSlots, coveredHeadSlots] = CalcCoveredSlots(params.armorSet->items, params);

    coveredSlots = 0;  // weird case where an item's in the armor set but not in the curve tree

    ProcessBaseArmorSet(params, coveredHeadSlots, [&](ArmorSlot slot, RE::TESObjectARMO*) { coveredSlots |= GetAllCoveredSlots(params.curve->tree, slot); });

    return coveredSlots;
}

void RemoveMemberIf(rapidjson::Value& val, const char* objName, const char* memberName, bool bRemove) {
    if (!bRemove) return;

    if (!val.HasMember(objName)) return;
    val[objName].RemoveMember(memberName);
}


void MergeMember(rapidjson::Value& into, rapidjson::Value& from, MemoryPoolAllocator<>& al, const char* name) {
    if (!from.HasMember(name)) return;
    auto& val = from[name];

    if (!val.IsObject()) {
        into.RemoveMember(name);
        into.AddMember(Value(name, al), val, al);
    } else {
        if (into.HasMember(name)) {
            auto& prev = into[name];

            if (prev.IsObject()) {
                for (auto& m : val.GetObj()) {
                    prev.RemoveMember(m.name);
                    prev.AddMember(m.name, m.value, al);
                }
            } else {
                into.RemoveMember(name);
                into.AddMember(Value(name, al), val, al);
            }
        } else {
            into.AddMember(Value(name, al), val, al);
        }
    }

    from.RemoveMember(name);
}

int QuickArmorRebalance::MakeArmorChanges(const ArmorChangeParams& params) {
    int nChanges = 0;
    auto& data = *params.data;

    if (data.items.empty()) return 0;
    data.bMixedSetDone = false;

    params.remapMask = 0;
    for (auto i : params.mapArmorSlots) params.remapMask |= (1 << i.first);

    Document doc;
    auto& al = doc.GetAllocator();

    std::map<RE::TESBoundObject*, Value> mapChanges;

    // Process some things only when there's a new conversion, the rest can merge with previous conversions
    if (params.armorSet) {
        // Build list of base items per slot
        auto [_discard, coveredHeadSlots] = CalcCoveredSlots(params.armorSet->items, params);
        auto [coveredSlots, coveredHeadSlotsChanges] = CalcCoveredSlots(data.items, params, true);

        SlotRelativeWeight slotValues[32];

        ProcessBaseArmorSet(params, coveredHeadSlots, [&](ArmorSlot slot, RE::TESObjectARMO* i) { slotValues[slot].item = i; });

        int totalWeight = 0;
        for (const auto& i : params.curve->tree)
            totalWeight += GetTotalWeight(&i, ~((ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kShield | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kAmulet |
                                                (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kRing));
        for (const auto& i : params.curve->tree) PropogateBaseValues(slotValues, nullptr, &i);
        for (const auto& i : params.curve->tree) CalcCoveredValues(slotValues, coveredSlots, &i);

        for (auto i : data.items) {
            Value changes(kObjectType);
            RE::TESBoundObject* objBase = nullptr;

            if (auto armor = i->As<RE::TESObjectARMO>()) {
                SlotRelativeWeight* itemBase = nullptr;
                int weight = 0;

                // Need to retrieve the original slots, or double-applying will loose data
                ArmorSlots slotsOrig = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
                ArmorSlots slotsRemapped = RemapSlots(slotsOrig, params);
                ArmorSlots slots = slotsRemapped;

                slots = PromoteHeadSlots(slots, coveredHeadSlotsChanges);

                while (slots) {
                    unsigned long slot;
                    _BitScanForward(&slot, slots);
                    slots &= slots - 1;

                    auto& v = slotValues[slot];
                    if (v.base && v.base->weightBase) {
                        if (!itemBase) itemBase = v.base;
                        weight += v.weightUsed;
                    }
                }

                if (itemBase) {
                    objBase = itemBase->item;

                    Value weights(kObjectType);
                    weights.AddMember("item", weight, al);
                    weights.AddMember("base", itemBase->weightBase, al);
                    weights.AddMember("set", totalWeight, al);
                    changes.AddMember("w", weights, al);
                }
            } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
                if (auto itemBase = params.armorSet->FindMatching(weap)) {
                    objBase = itemBase;
                }
            } else if (auto ammo = i->As<RE::TESAmmo>()) {
                if (auto itemBase = params.armorSet->FindMatching(ammo)) {
                    objBase = itemBase;
                }
            }

            if (objBase) {
                changes.AddMember("name", Value(i->GetName(), al), al);
                changes.AddMember("srcname", Value(objBase->GetName(), al), al);
                changes.AddMember("srcfile", Value(objBase->GetFile(0)->fileName, al), al);
                changes.AddMember("srcid", Value(GetFileId(objBase)), al);
                mapChanges[i] = std::move(changes);
            }
        }
    }

    for (auto i : data.items) {
        auto& changes = mapChanges[i];
        if (!changes.IsObject()) changes.SetObject();

        if (auto armor = i->As<RE::TESObjectARMO>()) {
            AddModification("armor", params.armor.rating, changes, al);
            AddModification("weight", params.armor.weight, changes, al, false, kFlatWeightStore);
            AddModification("warmth", params.armor.warmth, changes, al);
            if (g_Config.isFrostfallInstalled || g_Config.bShowFrostfallCoverage) changes.AddMember("coverage", Value(0.01f * params.armor.coverage), al);
            AddModification("value", params.value, changes, al);

            ArmorSlots slotsOrig = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
            ArmorSlots slotsRemapped = RemapSlots(slotsOrig, params);
            if (slotsRemapped != slotsOrig) changes.AddMember("slots", slotsRemapped, al);

            if (params.ench.strip) changes.AddMember("stripEnch", Value(params.ench.stripArmor), al);

        } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
            AddModification("damage", params.weapon.damage, changes, al);
            AddModification("weight", params.weapon.weight, changes, al, false, kFlatWeightStore);
            AddModification("speed", params.weapon.speed, changes, al);
            AddModification("stagger", params.weapon.stagger, changes, al);
            AddModification("value", params.value, changes, al);

            if (params.ench.strip) {
                if (weap->GetWeaponType() == RE::WEAPON_TYPE::kStaff)
                    changes.AddMember("stripEnch", Value(params.ench.stripStaves), al);
                else
                    changes.AddMember("stripEnch", Value(params.ench.stripWeapons), al);
            }
        } else if (auto ammo = i->As<RE::TESAmmo>()) {
            AddModification("damage", params.weapon.damage, changes, al);
            AddModification("weight", params.weapon.weight, changes, al, false, kFlatWeightStore);
            AddModification("value", params.value, changes, al);
        }

        AddModification("enchRate", params.ench.rate, changes, al, true);
        AddModification("enchPower", params.ench.power, changes, al, true);

        if (params.ench.pool) {
            changes.AddMember("enchPool", Value(params.ench.pool->name.c_str(), al), al);
            if (params.ench.poolRestrict)
                changes.AddMember("enchPoolBias", Value(1.0f), al);
            else
                changes.AddMember("enchPoolBias", Value(0.01f * params.ench.poolChance), al);
        }

        struct Recipe {
            static Value Params(const ArmorChangeParams::RecipeOptions& params, MemoryPoolAllocator<>& al) {
                Value recipe(kObjectType);
                switch (params.action) {
                    case ArmorChangeParams::eRecipeModify:
                        if (params.bNew) recipe.AddMember("new", Value(true), al);
                        if (params.bFree) recipe.AddMember("free", Value(true), al);
                        if (params.modeItems != ArmorChangeParams::eRequirementReplace) recipe.AddMember("opItems", Value(params.modeItems), al);
                        if (params.modePerks != ArmorChangeParams::eRequirementReplace) recipe.AddMember("opPerks", Value(params.modePerks), al);
                        if (!params.skipForms.empty()) {
                            Value arr(kArrayType);
                            for (auto i : params.skipForms) {
                                arr.PushBack(Value(QARFormID(i).c_str(), al), al);
                            }
                            recipe.AddMember("noForms", arr, al);
                        }
                        break;
                    case ArmorChangeParams::eRecipeRemove:
                        recipe.AddMember("remove", Value(true), al);
                        break;
                }

                return recipe;
            }
        };

        if (params.bModifyKeywords) changes.AddMember("keywords", Value(true), al);
        if (params.temper.bModify) {
            changes.AddMember("temper", Recipe::Params(params.temper, al), al);
        }
        if (params.craft.bModify) {
            changes.AddMember("craft", Recipe::Params(params.craft, al), al);
        }

        auto loot = MakeLootChanges(params, i, al);
        if (!loot.IsNull()) changes.AddMember("loot", loot, al);
    }

    std::map<RE::TESFile*, Value> mapFileChanges;
    for (auto& i : mapChanges) {
        Value* ls = &mapFileChanges[i.first->GetFile(0)];
        if (!ls->IsObject()) ls->SetObject();

        ls->AddMember(Value(std::to_string(GetFileId(i.first)).c_str(), al), i.second, al);
    }
    mapChanges.clear();

    for (auto& i : mapFileChanges) {
        int r = 0;
        ::AddPreferenceVariants(i.first, params, i.second, al, r);
        ::AddDynamicVariants(i.first, params, i.second, al);
    }

    for (auto& i : mapFileChanges) {
        ExportToDAV(i.first, i.second);

        std::filesystem::path path(std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/");
        std::filesystem::create_directories(path);

        path /= i.first->fileName;
        path += ".json";

        if (!ReadJSONFile(path, doc)) continue;

        if (!i.second.IsObject()) continue;

        for (auto& j : i.second.GetObj()) {
            if (!params.bMerge) {
                if (j.value.HasMember("srcid")) {
                    doc.RemoveMember(j.name);  // it doesn't automaticaly remove duplicates
                    doc.AddMember(j.name, j.value, al);
                }
            } else {
                if (doc.HasMember(j.name)) {
                    auto& prev = doc[j.name];
                    if (!prev.IsObject()) {
                        doc.RemoveMember(j.name);
                        if (j.value.HasMember("srcid")) doc.AddMember(j.name, j.value, al);
                        continue;
                    }

                    RemoveMemberIf(prev, "loot", "region", !params.region);
                    MergeMember(prev, j.value, al, "loot");

                    for (auto& m : j.value.GetObj()) {
                        prev.RemoveMember(m.name);
                        prev.AddMember(m.name, m.value, al);
                    }
                } else {
                    if (j.value.HasMember("srcid")) doc.AddMember(j.name, j.value, al);
                }
            }
        }

        if (!WriteJSONFile(path, doc)) {
            g_Config.strCriticalError = std::format(
                "Unable to write to file {}\n"
                "Path: {}\n"
                "Error: {}\n"
                "Changes cannot be saved so further use of QAR is disabled",
                path.filename().generic_string(), path.generic_string(), std::strerror(errno));
        }

        nChanges += ApplyChanges(i.first, doc.GetObj(), g_Config.permLocal);
    }

    if (!params.mapKeywordChanges.empty()) MakeKeywordChanges(params);

    return nChanges;
}

int QuickArmorRebalance::AddDynamicVariants(const ArmorChangeParams& params) {
    auto& data = *params.data;

    int r = 0;
    std::set<RE::TESFile*> files;

    for (auto i : data.items) {
        if (auto armor = i->As<RE::TESObjectARMO>()) {
            files.insert(armor->GetFile(0));
        }
    }

    for (auto mod : files) {
        Document doc;
        auto& al = doc.GetAllocator();

        std::filesystem::path path(std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/");
        path /= mod->fileName;
        path += ".json";

        if (!ReadJSONFile(path, doc)) continue;
        if (!doc.IsObject()) continue;

        r += ::AddDynamicVariants(mod, params, doc.GetObj(), al);
        ExportToDAV(mod, doc.GetObj(), true);
        WriteJSONFile(path, doc);
    }

    return r;
}

int ::AddDynamicVariants(const RE::TESFile* file, const ArmorChangeParams& params, rapidjson::Value& ls, MemoryPoolAllocator<>& al) {
    auto& data = *params.data;
    int r = 0;

    // First pass to clear out existing DV entries (or reset them if they will no longer apply)
    for (auto item : data.items) {
        if (item->GetFile(0) != file) continue;

        auto itemId = std::to_string(GetFileId(item));
        auto it = ls.FindMember(itemId.c_str());
        if (it == ls.MemberEnd()) continue;

        auto& vals = it->value;
        if (!vals.IsObject()) continue;  // Somethings broken

        vals.RemoveMember("dynamicVariants");
    }

    for (auto& dv : data.dvSets) {
        for (auto& set : dv.second) {
            if (set.second.size() < 2) continue;

            auto base = set.second[0];
            for (int i = 1; i < set.second.size(); i++) {
                auto item = set.second[i];

                // Should only operate on checked items - this is crappy performance but shouldn't be running much
                if (std::find(data.items.begin(), data.items.end(), item) == data.items.end()) continue;

                if (item->GetFile(0) != file) continue;
                if (item->GetFile(0) != base->GetFile(0)) continue;  // Not supported

                auto itemId = std::to_string(GetFileId(item));
                auto it = ls.FindMember(itemId.c_str());
                if (it == ls.MemberEnd()) continue;

                auto& vals = it->value;
                if (!vals.IsObject()) continue;  // Somethings broken

                Value dvVal(kObjectType);
                // dvVal.AddMember("base", Value(std::to_string(GetFileId(base)).c_str(), al), al);
                dvVal.AddMember("base", GetFileId(base), al);
                dvVal.AddMember("stage", Value(i), al);

                it = vals.FindMember("dynamicVariants");
                if (it == vals.MemberEnd()) {
                    Value dvs(kObjectType);
                    dvs.AddMember(Value(dv.first->name.c_str(), al), dvVal, al);
                    vals.AddMember("dynamicVariants", dvs, al);
                } else {
                    it->value.RemoveMember(dv.first->name.c_str());
                    it->value.AddMember(Value(dv.first->name.c_str(), al), dvVal, al);
                }
                r++;

                g_Data.modData[item->GetFile(0)]->bHasDynamicVariants = true;
            }
        }
    }

    return r;
}

int QuickArmorRebalance::RescanPreferenceVariants() {
    int r = 0;
    auto Rescan = [&](auto mod, auto path) {
        Document doc;

        if (!ReadJSONFile(path, doc, false)) return;

        if (doc.HasParseError() || !doc.IsObject()) return;

        ArmorChangeData data;
        ArmorChangeParams params(data);

        for (auto& i : doc.GetObj()) {
            RE::FormID id = atoi(i.name.GetString());
            if (auto item = RE::TESForm::LookupByID(GetFullId(mod, id))) {
                if (auto armor = item->As<RE::TESObjectARMO>()) data.items.push_back(armor);
            }
        }

        if (data.items.empty()) return;

        AnalyzeArmor(data.items, data.analyzeResults);
        if (::AddPreferenceVariants(mod, params, doc.GetObj(), doc.GetAllocator(), r)) {
            logger::trace("Updating {}", path.generic_string().c_str());
            WriteJSONFile(path, doc);
        }
    };

    ForChangesInFolder("local/", Rescan);

    return r;
}

void WritePrefrenceVariantValue(RE::TESObjectARMO* item, rapidjson::Value& ls, const char* var, bool val, MemoryPoolAllocator<>& al) {
    auto itemId = std::to_string(GetFileId(item));
    auto it = ls.FindMember(itemId.c_str());
    if (it == ls.MemberEnd()) return;

    auto& vals = it->value;
    if (!vals.IsObject()) return;  // Somethings broken

    it = vals.FindMember("preferenceVariants");
    if (it == vals.MemberEnd()) {
        Value pvs(kObjectType);
        pvs.AddMember(Value(var, al), Value(val), al);
        vals.AddMember("preferenceVariants", pvs, al);
    } else {
        it->value.RemoveMember(var);
        it->value.AddMember(Value(var, al), Value(val), al);
    }
}

bool ::AddPreferenceVariants(const RE::TESFile* file, const ArmorChangeParams& params, rapidjson::Value& ls, MemoryPoolAllocator<>& al, int& count) {
    auto& data = *params.data;
    bool bAny = false;

    for (auto item : data.items) {
        if (item->GetFile(0) != file) continue;

        auto itemId = std::to_string(GetFileId(item));
        auto it = ls.FindMember(itemId.c_str());
        if (it == ls.MemberEnd()) continue;

        auto& vals = it->value;
        if (!vals.IsObject()) continue;  // Somethings broken

        vals.RemoveMember("preferenceVariants");
    }

    std::map<std::size_t, RE::TESObjectARMO*> mapHashed;

    for (auto& pw : g_Config.mapPrefVariants) {
        auto it = data.analyzeResults.mapWordItems.find(pw.second.hash);
        if (it != data.analyzeResults.mapWordItems.end()) {
            if (mapHashed.empty()) {  // Build hash map for words to armor
                for (auto item : data.items) {
                    if (item->GetFile(0) != file) continue;
                    if (auto armor = item->As<RE::TESObjectARMO>()) {
                        auto itArmor = data.analyzeResults.mapArmorWords.find(armor);
                        if (itArmor != data.analyzeResults.mapArmorWords.end()) mapHashed[HashWordSet(itArmor->second, armor)] = armor;
                    }
                }
            }

            for (auto item : it->second.items) {
                if (item->GetFile(0) != file) continue;
                if (std::find(data.items.begin(), data.items.end(), static_cast<RE::TESBoundObject*>(item)) == data.items.end()) continue;

                auto itArmor = data.analyzeResults.mapArmorWords.find(item);
                if (itArmor != data.analyzeResults.mapArmorWords.end()) {
                    auto hash = HashWordSet(itArmor->second, item, pw.second.hash);

                    auto itMatch = mapHashed.find(hash);
                    if (itMatch != mapHashed.end()) {
                        bAny = true;
                        WritePrefrenceVariantValue(itMatch->second, ls, pw.first.c_str(), false, al);
                        WritePrefrenceVariantValue(item, ls, pw.first.c_str(), true, al);

                        count++;
                    }
                }
            }
        }
    }

    return bAny;
}

int QuickArmorRebalance::ApplyChanges(const RE::TESFile* file, const rapidjson::Value& ls, const Permissions& perm) {
    if (!ls.IsObject()) return 0;

    int nChanges = 0;
    unsigned int allChanges = 0;

    for (auto& i : ls.GetObj()) {
        if (!i.name.IsString()) {
            logger::error("Invalid item id in {}", file->fileName);
            continue;
        }

        unsigned int changed = 0;
        RE::FormID id = atoi(i.name.GetString());
        if (ApplyChanges(file, id, i.value, perm, changed)) {
            nChanges++;
            allChanges |= changed;
        } else
            logger::error("Failed to apply changes to {}:{:#08x}", file->fileName, id);
    }

    logger::trace("{}: {} changes made", file->fileName, nChanges);

    if (nChanges > 0) {
        g_Data.modData[file]->bModified = true;
        if (&perm == &g_Config.permShared) {
            g_Data.modifiedFilesShared[file] |= allChanges;
        } else {
            g_Data.modifiedFiles[file] |= allChanges;
            g_Data.modifiedFilesDeleted.erase(file);
        }
    }

    return nChanges;
}

template <class T, typename V>
bool ChangeField(bool& bChanged, bool bAllowed, const char* field, const rapidjson::Value& changes, T* src, T* item, V T::*member, const auto& fn, float wFlat = 1.0f,
                 float wSrc = 1.0f) {
    if (bAllowed && changes.HasMember(field)) {
        auto& jsonScale = changes[field];

        assert(jsonScale.IsInt() != jsonScale.IsFloat());  // logger::info("Both int and float!");

        if (jsonScale.IsFloat()) {
            auto scale = jsonScale.GetFloat();
            if (src->*member && scale > 0.0f)
                item->*member = fn(wSrc * scale * src->*member);
            else
                item->*member = 0;
        } else if (jsonScale.IsInt()) {
            auto flat = jsonScale.GetInt();
            item->*member = fn(std::max(wFlat * flat + wSrc * src->*member, 0.0f));
        } else
            return false;

        bChanged = true;
    }

    return true;
}

void GetMatchingKeywords(const std::set<RE::BGSKeyword*>& set, std::vector<RE::BGSKeyword*>& addKwds, const RE::BGSKeywordForm* src) {
    for (unsigned int i = 0; i < src->numKeywords; i++) {
        if (!src->keywords[i]) continue;
        if (set.contains(src->keywords[i])) addKwds.push_back(src->keywords[i]);
    }
}

void MatchKeywords(RE::BGSKeywordForm* item, std::vector<RE::BGSKeyword*>& addKwds, const auto& fn) {
    std::vector<RE::BGSKeyword*> delKwds;

    for (unsigned int i = 0; i < item->numKeywords; i++) {
        if (!item->keywords[i] || fn(item->keywords[i])) {
            if (addKwds.empty())
                delKwds.push_back(item->keywords[i]);
            else {
                item->keywords[i] = addKwds.back();
                addKwds.pop_back();
            }
        }
    }

    // A more efficient bulk add/remove function exists, but it's not showing up for me and I currently
    // don't feel like spending the time to figure out why
    while (!addKwds.empty()) {
        item->AddKeyword(addKwds.back());
        addKwds.pop_back();
    }

    while (!delKwds.empty()) {
        item->RemoveKeyword(delKwds.back());
        delKwds.pop_back();
    }
}

bool QuickArmorRebalance::ApplyChanges(const RE::TESFile* file, RE::FormID id, const rapidjson::Value& changes, const Permissions& perm, unsigned int& changed) {
    if (!changes.IsObject()) return false;

    auto dataHandler = RE::TESDataHandler::GetSingleton();

    if (!changes.HasMember("srcfile") || !changes.HasMember("srcid")) return false;

    auto& jsonSrcFile = changes["srcfile"];
    auto& jsonSrcId = changes["srcid"];

    if (!jsonSrcFile.IsString() || !jsonSrcId.IsUint()) return false;

    auto item = RE::TESForm::LookupByID(GetFullId(file, id));
    if (!item) {
        logger::info("Item not found {}:{:#08x}", file->fileName, id);
        return false;
    }

    auto bo = item->As<RE::TESBoundObject>();
    if (!bo) {
        logger::info("Item wrong type {}:{:#08x}", file->fileName, id);
        return false;
    }

    auto objSrc = dataHandler->LookupForm(jsonSrcId.GetUint(), jsonSrcFile.GetString());
    if (!objSrc) return true;

    auto boSrc = objSrc->As<RE::TESBoundObject>();
    if (!boSrc) {
        logger::info("Item wrong type {}:{:#08x}", objSrc->GetFile(0)->fileName, objSrc->GetLocalFormID());
        return false;
    }

    ObjEnchantParams* enchParams = nullptr;
    if (auto as = g_Config.GetArmorSetFor(boSrc)) {
        if (as->ench.enchPool) {
            enchParams = &g_Data.enchParams[bo];
            enchParams->base = as->ench;
            if (as->loot) enchParams->level = as->loot->level;
        }
    }

    changed |= eChange_Conversion;

    float weight = 1.0f;
    bool bAnyChanges = false;

    if (auto armor = item->As<RE::TESObjectARMO>()) {
        auto src = objSrc->As<RE::TESObjectARMO>();
        if (!src) return true;

        if (!changes.HasMember("w")) return false;
        auto& jsonW = changes["w"];

        int itemW = 0;
        int baseW = 0;
        int setW = 0;
        float wFlat = 0.0f;

        if (jsonW.IsNumber())
            weight = jsonW.GetFloat();
        else if (jsonW.IsObject()) {
            auto wObj = jsonW.GetObj();

            itemW = wObj["item"].GetInt();
            baseW = wObj["base"].GetInt();
            setW = wObj["set"].GetInt();

            weight = baseW > 0 ? (float)itemW / baseW : 0.0f;
            wFlat = setW > 0 ? (float)itemW / setW : 0.0f;
        } else
            return false;

        if (!ChangeField<RE::TESObjectARMO>(
                bAnyChanges, perm.bModifyArmorRating, "armor", changes, src, armor, &RE::TESObjectARMO::armorRating, [=](float f) { return std::max(1, (int)f); }, 100 * wFlat,
                weight))
            return false;

        if (!ChangeField<RE::TESObjectARMO>(
                bAnyChanges, perm.bModifyWeight, "weight", changes, src, armor, &RE::TESObjectARMO::weight,
                [=](float f) {
                    if (g_Config.bRoundWeight) f = std::max(0.1f, 0.1f * std::round(10.0f * f));
                    return f;
                },
                wFlat / kFlatWeightStore, weight))
            return false;

        if (!ChangeField<RE::TESObjectARMO>(
                bAnyChanges, perm.bModifyValue, "value", changes, src, armor, &RE::TESObjectARMO::value, [=](float f) { return std::max(1, (int)f); }, wFlat, weight))
            return false;

        if (perm.bModifySlots && changes.HasMember("slots")) {
            auto& jsonOption = changes["slots"];
            if (!jsonOption.IsUint()) return false;

            changed |= eChange_Slots;

            if (!g_Data.modifiedArmorSlots.contains(armor))  // Don't overwrite previous
                g_Data.modifiedArmorSlots[armor] = armor->bipedModelData.bipedObjectSlots.underlying();

            auto slots = (ArmorSlots)jsonOption.GetUint();
            armor->bipedModelData.bipedObjectSlots = (RE::BIPED_MODEL::BipedObjectSlot)slots;
            for (auto addon : armor->armorAddons) {
                if (addon->GetFile(0) == armor->GetFile(0)) {  // Don't match other files, might be placeholders
                    // Some models have multiple pieces with different slots, so leave parts alone that aren't changing
                    // This might need to be converted to a 2 pass setup

                    if ((addon->bipedModelData.bipedObjectSlots.underlying() & slots) == addon->bipedModelData.bipedObjectSlots.underlying()) {
                        slots &= ~addon->bipedModelData.bipedObjectSlots.underlying();
                        continue;
                    }

                    addon->bipedModelData.bipedObjectSlots = (RE::BIPED_MODEL::BipedObjectSlot)slots;

                    for (int i = 0; i < RE::SEXES::kTotal; i++) {
                        if (!addon->bipedModels[i].model.empty()) {
                            std::string modelPath(addon->bipedModels[i].model);
                            ToLower(modelPath);

                            auto hash = std::hash<std::string>{}(modelPath);
                            if (!g_Data.noModifyModels.contains(hash)) g_Data.remapFileArmorSlots[hash] = slots;

                            if (modelPath.length() > 6) {
                                char* pChar = modelPath.data() + modelPath.length() - 6;  //'_X.nif'
                                if (*pChar++ == '_') {
                                    if (*pChar == '0') {
                                        *pChar = '1';
                                        hash = std::hash<std::string>{}(modelPath);
                                        if (!g_Data.noModifyModels.contains(hash)) g_Data.remapFileArmorSlots[hash] = slots;
                                    } else if (*pChar == '1') {
                                        *pChar = '0';
                                        hash = std::hash<std::string>{}(modelPath);
                                        if (!g_Data.noModifyModels.contains(hash)) g_Data.remapFileArmorSlots[hash] = slots;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        bool bModifyFFKeywords = false;
        std::vector<RE::BGSKeyword*> addKwds;

        if (perm.bModifyWarmth && changes.HasMember("warmth") && setW > 0) {
            float warmth = 0.0f;
            auto& jsonScale = changes["warmth"];
            if (jsonScale.IsNumber()) {
                auto scale = warmth = jsonScale.GetFloat();
                g_Data.modifiedWarmth[armor] = scale * itemW / setW * 150.0f;
                changed |= eChange_Survival;
            }

            if (g_Config.isFrostfallInstalled && changes.HasMember("coverage")) {
                changed |= eChange_Survival;
                auto coverage = changes["coverage"].GetFloat();
                bModifyFFKeywords = true;

                constexpr auto ffSlots = (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kBody | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead |
                                         (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kFeet | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHands;

                if ((ArmorSlots)armor->GetSlotMask() & ffSlots) {
                    constexpr float warmthMin[] = {1.0f, 0.9f, 0.7f, 0.55f, 0.3f};
                    constexpr float coverageMin[] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};

                    int nWarmth = 0;
                    int nCoverage = 0;
                    for (; nWarmth < 5; nWarmth++) {
                        if (warmth >= warmthMin[nWarmth]) break;
                    }
                    for (; nCoverage < 5; nCoverage++) {
                        if (coverage >= coverageMin[nCoverage]) break;
                    }

                    addKwds.push_back(g_Config.ffKeywords.enable);
                    if (nWarmth < 5 && nCoverage < 5) {
                        addKwds.push_back(g_Config.ffKeywords.warmth[4 - nWarmth]);
                        addKwds.push_back(g_Config.ffKeywords.coverage[4 - nCoverage]);
                    } else
                        addKwds.push_back(g_Config.ffKeywords.ignore);
                } else
                    addKwds.push_back(g_Config.ffKeywords.ignore);
            }
        }

        if (perm.bModifyKeywords && changes.HasMember("keywords")) {
            auto& jsonOption = changes["keywords"];
            if (!jsonOption.IsBool()) return false;

            if (jsonOption.GetBool()) {
                logger::trace("Changing keywords");
                bAnyChanges = true;
                armor->bipedModelData.armorType = src->bipedModelData.armorType;

                GetMatchingKeywords(g_Config.kwSet, addKwds, src);

                if ((unsigned int)src->GetSlotMask() & (unsigned int)armor->GetSlotMask()) {
                    GetMatchingKeywords(g_Config.kwSlotSpecSet, addKwds, src);
                }

                MatchKeywords(armor, addKwds, [=](RE::BGSKeyword* kw) {
                    return g_Config.kwSet.contains(kw) || g_Config.kwSlotSpecSet.contains(kw) || (bModifyFFKeywords && g_Config.kwFFSet.contains(kw));
                });
            }
        }

        if (changes.HasMember("dynamicVariants")) {
            // Don't need to use the data currently, just mark it
            g_Data.modData[armor->GetFile(0)]->bHasDynamicVariants = true;
        }

        if (g_Data.loot && changes.HasMember("preferenceVariants")) {
            auto& jsonVarts = changes["preferenceVariants"];
            if (!jsonVarts.IsObject()) return false;

            for (const auto& pv : g_Config.mapPrefVariants) {
                if (jsonVarts.HasMember(pv.first.c_str())) {
                    if (jsonVarts[pv.first.c_str()].GetBool()) {
                        g_Data.loot->prefVartWith[pv.second.hash].insert(armor);
                    } else
                        g_Data.loot->prefVartWithout[pv.second.hash].insert(armor);
                }
            }
        }

        if (perm.bStripEnchArmor && GetJsonBool(changes, "stripEnch")) {
            bAnyChanges = true;
            armor->formEnchanting = nullptr;
        }

    } else if (auto weap = item->As<RE::TESObjectWEAP>()) {
        auto src = objSrc->As<RE::TESObjectWEAP>();
        if (!src) return true;

        if (!ChangeField<RE::TESObjectWEAP>(bAnyChanges, perm.bModifyWeapDamage, "damage", changes, src, weap, &RE::TESObjectWEAP::attackDamage,
                                            [=](float f) { return (uint16_t)std::max(1, (int)f); }))
            return false;

        if (!ChangeField<RE::TESObjectWEAP::CriticalData>(bAnyChanges, perm.bModifyWeapDamage, "damage", changes, &src->criticalData, &weap->criticalData,
                                                          &RE::TESObjectWEAP::CriticalData::damage, [=](float f) { return (uint16_t)std::max(1, (int)f); }))
            return false;
        if (perm.bModifyWeapDamage) weap->criticalData.prcntMult = src->criticalData.prcntMult;

        if (!ChangeField<RE::TESObjectWEAP>(
                bAnyChanges, perm.bModifyWeapWeight, "weight", changes, src, weap, &RE::TESObjectWEAP::weight,
                [=](float f) {
                    f *= weight;
                    if (g_Config.bRoundWeight) f = std::max(0.1f, 0.1f * std::round(10.0f * f));
                    return f;
                },
                1.0f / kFlatWeightStore))
            return false;

        if (!ChangeField<RE::TESObjectWEAP::Data>(bAnyChanges, perm.bModifyWeapSpeed, "speed", changes, &src->weaponData, &weap->weaponData, &RE::TESObjectWEAP::Data::speed,
                                                  [=](float f) { return f; }))
            return false;

        if (!ChangeField<RE::TESObjectWEAP::Data>(bAnyChanges, perm.bModifyWeapStagger, "stagger", changes, &src->weaponData, &weap->weaponData,
                                                  &RE::TESObjectWEAP::Data::staggerValue, [=](float f) { return f; }))
            return false;

        if (!ChangeField<RE::TESObjectWEAP>(bAnyChanges, perm.bModifyValue, "value", changes, src, weap, &RE::TESObjectWEAP::value, [=](float f) { return std::max(1, (int)f); }))
            return false;

        if (perm.bModifyKeywords && changes.HasMember("keywords")) {
            auto& jsonOption = changes["keywords"];
            if (!jsonOption.IsBool()) return false;

            if (jsonOption.GetBool()) {
                logger::trace("Changing keywords");
                bAnyChanges = true;

                std::vector<RE::BGSKeyword*> addKwds;

                GetMatchingKeywords(g_Config.kwSetWeap, addKwds, src);
                MatchKeywords(weap, addKwds, [](RE::BGSKeyword* kw) { return g_Config.kwSetWeap.contains(kw); });
            }
        }

        if (weap->GetWeaponType() == RE::WEAPON_TYPE::kStaff) {
            if (perm.bStripEnchStaves && GetJsonBool(changes, "stripEnch")) {
                bAnyChanges = true;
                weap->formEnchanting = nullptr;
                weap->amountofEnchantment = 0;
            }

            if (auto enchGroup = MapFindOrNull(g_Data.staffEnchGroup, src)) g_Data.staffEnchGroup[weap] = enchGroup;
        } else {
            if (perm.bStripEnchWeapons && GetJsonBool(changes, "stripEnch")) {
                bAnyChanges = true;
                weap->formEnchanting = nullptr;
                weap->amountofEnchantment = 0;
            }
        }

    } else if (auto ammo = item->As<RE::TESAmmo>()) {
        auto src = objSrc->As<RE::TESAmmo>();
        if (!src) return true;

        if (!ChangeField<RE::AMMO_DATA>(bAnyChanges, perm.bModifyWeapDamage, "damage", changes, &src->GetRuntimeData().data, &ammo->GetRuntimeData().data, &RE::AMMO_DATA::damage,
                                        [=](float f) { return std::max(1.0f, f); }))
            return false;

        if (!ChangeField<RE::TESAmmo>(bAnyChanges, perm.bModifyValue, "value", changes, src, ammo, &RE::TESAmmo::value, [=](float f) { return std::max(1, (int)f); })) return false;

        if (perm.bModifyKeywords && changes.HasMember("keywords")) {
            auto& jsonOption = changes["keywords"];
            if (!jsonOption.IsBool()) return false;

            if (jsonOption.GetBool()) {
                logger::trace("Changing keywords");
                bAnyChanges = true;

                std::vector<RE::BGSKeyword*> addKwds;

                GetMatchingKeywords(g_Config.kwSetWeap, addKwds, src->AsKeywordForm());
                MatchKeywords(ammo->AsKeywordForm(), addKwds, [](RE::BGSKeyword* kw) { return g_Config.kwSetWeap.contains(kw); });
            }
        }
    }

    if (bAnyChanges) changed |= eChange_Stats;

    int rarity = -1; //Needed for crafting filters
    if (perm.bDistributeLoot) {
        if (changes.HasMember("loot")) {
            auto& jsonLoot = changes["loot"];
            if (!jsonLoot.IsObject()) return false;

            if (jsonLoot.HasMember("rarity") && jsonLoot["rarity"].IsInt()) rarity = jsonLoot["rarity"].GetInt();

            changed |= eChange_Loot;
            if (g_Data.loot) LoadLootChanges(bo, jsonLoot, changed);
        }
    }

    if (enchParams) {
        if (changes.HasMember("enchRate")) {
            auto& jsonVal = changes["enchRate"];
            if (jsonVal.IsNumber()) enchParams->unique.enchRate = std::clamp(jsonVal.GetFloat(), 0.0f, 5.0f);
        }
        if (changes.HasMember("enchPower")) {
            auto& jsonVal = changes["enchPower"];
            if (jsonVal.IsNumber()) enchParams->unique.enchPower = std::clamp(jsonVal.GetFloat(), 0.1f, 3.0f);
        }

        if (changes.HasMember("enchPool")) {
            auto& jsonVal = changes["enchPool"];
            if (jsonVal.IsString()) {
                auto hash = std::hash<std::string>{}(MakeLower(jsonVal.GetString()));

                enchParams->unique.enchPool = MapFind(g_Config.mapEnchPools, hash);
            }
        }

        if (enchParams->unique.enchPool && changes.HasMember("enchPoolBias")) {
            auto& jsonVal = changes["enchPoolBias"];
            if (jsonVal.IsNumber()) enchParams->uniquePoolChance = std::clamp(jsonVal.GetFloat(), 0.0f, 1.0f);
        }
    }

    struct RecipeSettings {
        static void Read(const rapidjson::Value& json, ArmorChangeParams::RecipeOptions& opts) {
            opts.modeItems = GetJsonInt(json, "opItems", 0, ArmorChangeParams::eRequirementModeCount - 1, ArmorChangeParams::eRequirementReplace);
            opts.modePerks = GetJsonInt(json, "opPerks", 0, ArmorChangeParams::eRequirementModeCount - 1, ArmorChangeParams::eRequirementReplace);

            if (json.HasMember("noForms") && json["noForms"].IsArray()) {
                auto fileDefault = RE::TESDataHandler::GetSingleton()->LookupLoadedModByIndex(0);
                for (auto& i : json["noForms"].GetArray()) {
                    if (i.IsString()) {
                        if (auto form = FindIn(fileDefault, i.GetString())) {
                            opts.skipForms.insert(form);
                        }
                    }
                }
            }
        }
    };

    if (perm.temper.bModify && changes.HasMember("temper") && !item->As<RE::TESAmmo>()) {
        auto& jsonOpts = changes["temper"];
        if (!jsonOpts.IsObject()) return false;

        changed |= eChange_Recipes;

        auto opts = jsonOpts.GetObj();

        bool bRemove = false;
        if (opts.HasMember("remove")) bRemove = opts["remove"].GetBool();

        if (!bRemove) {
            bool bNew = false;
            bool bFree = false;

            if (opts.HasMember("new")) bNew = opts["new"].GetBool();
            if (opts.HasMember("free")) bFree = opts["free"].GetBool();

            ArmorChangeParams::RecipeOptions recipeOpts;
            RecipeSettings::Read(opts, recipeOpts);

            RE::BGSConstructibleObject *recipeItem = nullptr, *recipeSrc = nullptr;

            auto it = g_Data.temperRecipe.find(bo);
            if (it != g_Data.temperRecipe.end()) recipeItem = it->second;

            it = g_Data.temperRecipe.find(boSrc);
            if (it != g_Data.temperRecipe.end()) recipeSrc = it->second;

            bFree = (bFree && perm.crafting.bFree) || recipeSrc;  // If recipe source, free doesn't matter

            if (!recipeItem && bNew && perm.temper.bCreate && bFree) {
                auto newForm = static_cast<RE::BGSConstructibleObject*>(RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

                if (newForm) {
                    newForm->benchKeyword = recipeSrc ? recipeSrc->benchKeyword
                                                      : (item->As<RE::TESObjectARMO>() ? RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingArmorTable")
                                                                                       : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingSharpeningWheel"));
                    newForm->createdItem = item;
                    newForm->data.numConstructed = 1;

                    dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                    g_Data.temperRecipe.insert({bo, recipeItem});
                    recipeItem = newForm;
                }
            }

            if (recipeItem && bFree) ::ReplaceRecipe(recipeOpts, recipeItem, recipeSrc, weight, g_Config.fTemperGoldCostRatio);
        } else {
            if (perm.temper.bRemove) {
                if (auto recipeItem = MapFindOrNull(g_Data.temperRecipe, bo)) {
                    recipeItem->benchKeyword = nullptr;
                }
            }
        }
    }

    if (perm.crafting.bModify && changes.HasMember("craft")) {
        auto& jsonOpts = changes["craft"];
        if (!jsonOpts.IsObject()) return false;

        changed |= eChange_Recipes;

        auto opts = jsonOpts.GetObj();

        bool bRemove = false;
        if (opts.HasMember("remove")) bRemove = opts["remove"].GetBool();

        if (!bRemove) {
            bool bNew = false;
            bool bFree = false;

            if (opts.HasMember("new")) bNew = opts["new"].GetBool();
            if (opts.HasMember("free")) bFree = opts["free"].GetBool();

            ArmorChangeParams::RecipeOptions recipeOpts;
            RecipeSettings::Read(opts, recipeOpts);

            RE::BGSConstructibleObject *recipeItem = nullptr, *recipeSrc = nullptr;

            auto it = g_Data.craftRecipe.find(bo);
            if (it != g_Data.craftRecipe.end()) recipeItem = it->second;

            it = g_Data.craftRecipe.find(boSrc);
            if (it != g_Data.craftRecipe.end()) recipeSrc = it->second;

            bFree = (bFree && perm.crafting.bFree) || recipeSrc;  // If recipe source, free doesn't matter

            if (rarity <= g_Config.craftingRarityMax) {
                if (!recipeItem && bNew && perm.crafting.bCreate && bFree) {
                    auto newForm = static_cast<RE::BGSConstructibleObject*>(RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

                    if (newForm) {
                        newForm->benchKeyword = recipeSrc ? recipeSrc->benchKeyword : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingForge");
                        newForm->createdItem = item;
                        newForm->data.numConstructed = item->As<RE::TESAmmo>() ? 24 : 1;

                        dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(
                            newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                        g_Data.craftRecipe.insert({bo, recipeItem});
                        recipeItem = newForm;
                    }
                }

                if (recipeItem && bFree) ::ReplaceRecipe(recipeOpts, recipeItem, recipeSrc, weight, g_Config.fCraftGoldCostRatio);
            } else if (g_Config.bDisableCraftingRecipesOnRarity && recipeItem) {
                logger::trace("Disabling recipe for {}", item->GetName());
                recipeItem->benchKeyword = nullptr;
            }
        } else {
            if (perm.crafting.bRemove) {
                if (auto recipeItem = MapFindOrNull(g_Data.craftRecipe, bo)) {
                    recipeItem->benchKeyword = nullptr;
                }
            }
        }
    }

    if (g_Config.bEnableSmeltingRecipes) {
        RE::BGSConstructibleObject *recipeItem = nullptr, *recipeSrc = nullptr;

        auto it = g_Data.smeltRecipe.find(bo);
        if (it != g_Data.smeltRecipe.end()) recipeItem = it->second;

        it = g_Data.smeltRecipe.find(boSrc);
        if (it != g_Data.smeltRecipe.end()) recipeSrc = it->second;

        if (recipeSrc) {
            if (!recipeItem) {
                auto newForm = static_cast<RE::BGSConstructibleObject*>(RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

                if (newForm) {
                    newForm->benchKeyword = recipeSrc ? recipeSrc->benchKeyword : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmelter");

                    dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                    g_Data.smeltRecipe.insert({bo, recipeItem});
                    recipeItem = newForm;
                }

            } else
                ::ClearRecipe(recipeItem);

            if (recipeItem) {
                ::CopyConditions(recipeItem->conditions, recipeSrc->conditions, recipeSrc->requiredItems.containerObjects[0]->obj, bo);

                auto& mats = recipeItem->requiredItems;
                mats.containerObjects = RE::calloc<RE::ContainerObject*>(mats.numContainerObjects = 1);
                mats.containerObjects[0] = new RE::ContainerObject(bo, recipeSrc->requiredItems.containerObjects[0]->count);

                recipeItem->data.numConstructed = (uint16_t)std::max(1, (int)(weight * recipeSrc->data.numConstructed));
                recipeItem->createdItem = recipeSrc->createdItem;
            }
        }
    }

    //Record changes
    if (changed) {
        if (&perm == &g_Config.permShared)
            g_Data.modifiedItemsShared[bo] |= changed;
        else {
            g_Data.modifiedItems[bo] |= changed;
            g_Data.modifiedItemsDeleted.erase(bo);
        }
    }

    return true;
}

///////////////////////////////////////////////
// Recipes

// Old, but still using for smelting recipes
void ::CopyConditions(RE::TESCondition& tar, const RE::TESCondition& src, void* replace, void* replaceWith) {
    auto prev = tar.head;
    if (prev)
        while (prev->next) prev = prev->next;

    if (src.head) {
        for (auto psrc = src.head; psrc; psrc = psrc->next) {
            bool bSkip = false;
            switch (psrc->data.functionData.function.get()) {
                case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
                case RE::FUNCTION_DATA::FunctionID::kGetEquipped:
                case RE::FUNCTION_DATA::FunctionID::kHasPerk:
                    bSkip = g_Config.recipeConditionBlacklist.contains((RE::TESForm*)psrc->data.functionData.params[0]);
                    break;
            }

            if (bSkip) {
                if (prev) prev->data.flags.isOR = false;
                continue;
            }

            auto p = new RE::TESConditionItem();

            if (!prev)
                tar.head = p;
            else
                prev->next = p;

            p->data = psrc->data;

            switch (p->data.functionData.function.get()) {
                case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
                case RE::FUNCTION_DATA::FunctionID::kGetEquipped:
                    if (p->data.functionData.params[0] == replace) p->data.functionData.params[0] = replaceWith;
                    break;
            }
            p->next = nullptr;
            prev = p;
        }
    }
}

static void ClearMats(RE::BGSConstructibleObject* tar) {
    auto& mats = tar->requiredItems;

    if (mats.containerObjects) {
        for (unsigned int i = 0; i < mats.numContainerObjects; i++) delete mats.containerObjects[i];
        RE::free(mats.containerObjects);
    }

    mats.containerObjects = nullptr;
    mats.numContainerObjects = 0;
}

static void DeleteChain(RE::TESConditionItem* cond) {
    while (cond) {
        auto next = cond->next;
        delete cond;
        cond = next;
    }
}

// Old, but still using for smelting recipes
void ::ClearRecipe(RE::BGSConstructibleObject* tar) {
    ClearMats(tar);

    {  // Conditions
        RE::TESConditionItem* book = nullptr;
        auto& conds = tar->conditions;

        for (auto head = conds.head; head;) {
            auto next = head->next;

            if (QuickArmorRebalance::g_Config.bKeepCraftingBooks && head->data.functionData.function == RE::FUNCTION_DATA::FunctionID::kGetItemCount) {
                head->next = book;
                head->data.flags.isOR = false;
                book = head;
            } else
                delete head;

            head = next;
        }
        conds.head = book;
    }
}

inline bool KeepCondition(RE::BGSConstructibleObject* recipe, RE::TESConditionItem* cond, const ArmorChangeParams::RecipeOptions& opts, bool isOrig, RE::TESForm* replacing) {
    if (isOrig) {
        switch (cond->data.functionData.function.get()) {
            case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
            case RE::FUNCTION_DATA::FunctionID::kGetEquipped: {
                auto form = (RE::TESBoundObject*)cond->data.functionData.params[0];
                if (g_Config.bKeepCraftingBooks && recipe->requiredItems.CountObjectsInContainer(form) == 0 && form->As<RE::TESObjectBOOK>()) return true;
                if (opts.modeItems != ArmorChangeParams::eRequirementKeep) return false;
                if (recipe->requiredItems.CountObjectsInContainer(form) > 0) return false;  // Need to clear materials
                if (opts.skipForms.contains(form)) return false;
                if (g_Config.recipeConditionBlacklist.contains(form)) return false;
                return true;
            }
            case RE::FUNCTION_DATA::FunctionID::kHasPerk: {
                if (opts.modePerks != ArmorChangeParams::eRequirementKeep) return false;
                auto form = (RE::TESForm*)cond->data.functionData.params[0];
                if (opts.skipForms.contains(form)) return false;
                if (g_Config.recipeConditionBlacklist.contains(form)) return false;
                return true;
            } break;
            default:
                return false;
        }
    } else {
        switch (cond->data.functionData.function.get()) {
            case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
            case RE::FUNCTION_DATA::FunctionID::kGetEquipped: {
                auto form = (RE::TESBoundObject*)cond->data.functionData.params[0];
                if (form == replacing) return true;
                if (recipe->requiredItems.CountObjectsInContainer((RE::TESBoundObject*)form) > 0) return true;  // Always keep materials
                if (opts.modeItems != ArmorChangeParams::eRequirementReplace) return false;
                if (opts.skipForms.contains(form)) return false;
                if (g_Config.recipeConditionBlacklist.contains(form)) return false;
                return true;
            }
            case RE::FUNCTION_DATA::FunctionID::kHasPerk: {
                if (opts.modePerks != ArmorChangeParams::eRequirementReplace) return false;
                auto form = (RE::TESForm*)cond->data.functionData.params[0];
                if (opts.skipForms.contains(form)) return false;
                if (g_Config.recipeConditionBlacklist.contains(form)) return false;
                return true;
            } break;
            default:
                return true;
        }
    }
}

inline RE::TESConditionItem* CopyCondition(RE::BGSConstructibleObject* recipe, RE::TESConditionItem* cond, RE::TESForm* replacing, RE::TESConditionItem** pHead,
                                           RE::TESConditionItem* pTail) {
    auto p = new RE::TESConditionItem();

    if (!pTail)
        *pHead = p;
    else
        pTail->next = p;

    p->data = cond->data;

    switch (p->data.functionData.function.get()) {
        case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
        case RE::FUNCTION_DATA::FunctionID::kGetEquipped:
            if (p->data.functionData.params[0] == replacing) p->data.functionData.params[0] = recipe->createdItem;
            break;
    }
    p->next = nullptr;
    return p;
}

inline RE::TESConditionItem* CopyConditionsIter(RE::BGSConstructibleObject* recipe, RE::TESConditionItem* cond, const ArmorChangeParams::RecipeOptions& opts, bool isOrig,
                                                RE::TESForm* replacing, RE::TESConditionItem** pHead, RE::TESConditionItem** pTail) {
    if (!cond) return nullptr;

    if (cond->data.flags.isOR) {
        bool bAll = true;
        RE::TESConditionItem* last = cond;
        for (auto i = cond; i; i = i->next) {
            last = i;
            if (!bAll || !KeepCondition(recipe, i, opts, isOrig, replacing)) {
                bAll = false;
            }

            if (!i->data.flags.isOR) break;
        }

        last = last->next;
        if (!bAll) return last;

        for (auto i = cond; i != last; i = i->next) {
            *pTail = CopyCondition(recipe, i, replacing, pHead, *pTail);
        }

        (*pTail)->data.flags.isOR = false;

        return last;
    } else {
        if (KeepCondition(recipe, cond, opts, isOrig, replacing)) {
            *pTail = CopyCondition(recipe, cond, replacing, pHead, *pTail);
        }
        return cond->next;
    }
}

inline void CopyConditions(RE::BGSConstructibleObject* recipe, RE::TESConditionItem* cond, const ArmorChangeParams::RecipeOptions& opts, bool isOrig, RE::TESForm* replacing,
                           RE::TESConditionItem** pHead, RE::TESConditionItem** pTail) {
    while (cond) {
        cond = CopyConditionsIter(recipe, cond, opts, isOrig, replacing, pHead, pTail);
    }
}

void ::ReplaceRecipe(ArmorChangeParams::RecipeOptions& opts, RE::BGSConstructibleObject* tar, const RE::BGSConstructibleObject* src, float w, float fCost) {
    ClearMats(tar);

    RE::TESConditionItem* pHead = nullptr;
    RE::TESConditionItem* pTail = nullptr;

    CopyConditions(tar, tar->conditions.head, opts, true, tar->createdItem, &pHead, &pTail);

    if (src) {
        tar->benchKeyword = src->benchKeyword;
        tar->data.numConstructed = src->data.numConstructed;

        {  // Materials
            const auto& srcmats = src->requiredItems;
            auto& mats = tar->requiredItems;

            if (src->requiredItems.numContainerObjects > 0) {
                mats.containerObjects = RE::calloc<RE::ContainerObject*>(srcmats.numContainerObjects);
                mats.numContainerObjects = srcmats.numContainerObjects;

                for (unsigned int i = 0; i < srcmats.numContainerObjects; i++) {
                    mats.containerObjects[i] = new RE::ContainerObject(srcmats.containerObjects[i]->obj, std::max(1, (int)(w * srcmats.containerObjects[i]->count)));
                }
            }
        }

        CopyConditions(tar, src->conditions.head, opts, false, src->createdItem, &pHead, &pTail);
    } else if (fCost > 0) {
        auto& mats = tar->requiredItems;

        auto goldForm = RE::TESForm::LookupByID(0xf);
        if (!goldForm) return;

        auto goldObj = goldForm->As<RE::TESBoundObject>();
        if (!goldObj) return;

        mats.containerObjects = RE::calloc<RE::ContainerObject*>(mats.numContainerObjects = 1);
        mats.containerObjects[0] = new RE::ContainerObject(goldObj, std::max(1, (int)(0.01f * fCost * tar->createdItem->GetGoldValue())));
    }

    DeleteChain(tar->conditions.head);
    tar->conditions.head = pHead;
}

//~Recipes
//////////////////////////////

void QuickArmorRebalance::DeleteChanges(std::set<RE::TESBoundObject*> items, const char** fields) {
    std::map<RE::TESFile*, std::vector<RE::TESBoundObject*>> mapFileItems;
    for (auto i : items) {
        if (g_Data.modifiedItems.contains(i)) mapFileItems[i->GetFile(0)].push_back(i);
    }

    for (auto& i : mapFileItems) {
        Document doc;

        std::filesystem::path path(std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/");
        path /= i.first->fileName;
        path += ".json";

        if (!ReadJSONFile(path, doc)) continue;
        if (!doc.IsObject()) continue;

        if (fields) {
            auto ls = doc.GetObj();
            for (auto item : i.second) {
                auto it = ls.FindMember(std::to_string(GetFileId(item)).c_str());
                if (it == ls.MemberEnd()) continue;

                auto& vals = it->value;
                if (!vals.IsObject()) continue;  // Somethings broken

                for (auto field = fields; *field; field++) vals.RemoveMember(*field);
            }
        } else {
            for (auto item : i.second) {
                doc.RemoveMember(std::to_string(GetFileId(item)).c_str());
                g_Data.modifiedItemsDeleted.insert(item);
            }
        }

        WriteJSONFile(path, doc);
    }
}

/////////////////////////////////////////////
// Keywords

void ApplyKeywordChanges(const KeywordChangeMap& changes) {
    std::unordered_map<RE::TESBoundObject*, std::pair<std::vector<RE::BGSKeyword*>, std::vector<RE::BGSKeyword*>>> itemChangeKWs;

    for (auto& kwChanges : changes) {
        for (auto i : kwChanges.second.add) itemChangeKWs[i].first.push_back(kwChanges.first);
        for (auto i : kwChanges.second.remove) itemChangeKWs[i].second.push_back(kwChanges.first);
    }

    for (auto& item : itemChangeKWs) {
        MatchKeywords(item.first->As<RE::BGSKeywordForm>(), item.second.first,
                      [=](RE::BGSKeyword* kw) { return std::find(item.second.second.begin(), item.second.second.end(), kw) != item.second.second.end(); });
    }
}

void LoadKeywordChanges(const RE::TESFile* mod, const rapidjson::Value& ls, KeywordChangeMap& ret) {
    if (ls.IsObject()) {
        for (auto& jsonKW : ls.GetObj()) {
            if (!jsonKW.value.IsObject()) continue;

            auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(jsonKW.name.GetString());
            if (!kw) continue;

            auto& ckw = ret[kw];

            if (jsonKW.value.HasMember("add")) {
                auto& itemList = jsonKW.value["add"];
                if (!itemList.IsArray()) continue;

                for (auto& id : itemList.GetArray()) {
                    if (!id.IsUint()) continue;
                    auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(GetFullId(mod, id.GetUint()));
                    if (item) ckw.add.insert(item);
                }
            }

            if (jsonKW.value.HasMember("remove")) {
                auto& itemList = jsonKW.value["remove"];
                if (!itemList.IsArray()) continue;

                for (auto& id : itemList.GetArray()) {
                    if (!id.IsUint()) continue;
                    auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(GetFullId(mod, id.GetUint()));
                    if (item) ckw.remove.insert(item);
                }
            }
        }
    }
}

bool QuickArmorRebalance::LoadKeywordChanges(const RE::TESFile* file, std::filesystem::path path) {
    Document doc;
    if (!ReadJSONFile(path, doc)) return false;

    KeywordChangeMap changes;
    ::LoadKeywordChanges(file, doc, changes);
    ApplyKeywordChanges(changes);
    return true;
}

KeywordChangeMap QuickArmorRebalance::LoadKeywordChanges(const ArmorChangeParams& params) {
    auto& data = *params.data;
    std::set<RE::TESFile*> files;
    for (auto item : data.items) {
        files.insert(item->GetFile(0));
    }

    const std::filesystem::path path(std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/" PATH_CUSTOMKEYWORDS);

    KeywordChangeMap prevChanges;

    for (auto file : files) {
        auto filepath = path / file->fileName;
        filepath += ".json";

        Document doc;

        if (!ReadJSONFile(filepath, doc)) continue;
        ::LoadKeywordChanges(file, doc, prevChanges);
    }

    return prevChanges;
}

int QuickArmorRebalance::MakeKeywordChanges(const ArmorChangeParams& params, bool bApply) {
    auto& data = *params.data;
    std::set<RE::TESFile*> files;
    for (auto item : data.items) {
        files.insert(item->GetFile(0));
    }

    const std::filesystem::path path(std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/" PATH_CUSTOMKEYWORDS);
    std::filesystem::create_directories(path);

    for (auto file : files) {
        auto filepath = path / file->fileName;
        filepath += ".json";

        Document doc;
        auto& al = doc.GetAllocator();

        if (!ReadJSONFile(filepath, doc)) continue;

        KeywordChangeMap prevChanges;
        ::LoadKeywordChanges(file, doc, prevChanges);

        // To merge with previous changes, we basicaly have to seperate out untouched previous changes (items not enabled or keywords not present)
        // More or less brute forcing all this, not efficient but not sure there's a better way either
        // Often will be deleting everything one by one only to re-add it
        //
        // Purge any changes to existing items
        for (auto item : data.items) {
            if (item->GetFile(0) != file) continue;

            for (auto& i : prevChanges) {
                i.second.add.erase(item);
                i.second.remove.erase(item);
            }
        }

        // Merge into existing data
        for (auto& i : params.mapKeywordChanges) {
            auto& prev = prevChanges[i.first];
            for (auto item : i.second.add) {
                if (item->GetFile(0) == file) prev.add.insert(item);
            }
            for (auto item : i.second.remove) {
                if (item->GetFile(0) == file) prev.remove.insert(item);
            }
        }

        // Purge existing keyword entries
        // This is needed if an item has previously added keywords removed from it - it just wont have an entry anymore
        auto objKWs = doc.GetObj();
        for (auto it = objKWs.MemberBegin(); it != objKWs.MemberEnd();) {
            auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(it->name.GetString());
            if (!kw)
                it++;
            else
                it = doc.RemoveMember(it);
        }

        for (auto& i : prevChanges) {
            if (i.second.add.empty() && i.second.remove.empty()) continue;

            Value kwPair(kObjectType);

            if (!i.second.add.empty()) {
                Value arAdd(kArrayType);
                arAdd.Reserve((SizeType)i.second.add.size(), al);
                for (auto j : i.second.add) arAdd.PushBack(Value(GetFileId(j)), al);
                kwPair.AddMember("add", arAdd, al);
            }

            if (!i.second.remove.empty()) {
                Value arAdd(kArrayType);
                arAdd.Reserve((SizeType)i.second.add.size(), al);
                for (auto j : i.second.remove) arAdd.PushBack(Value(GetFileId(j)), al);
                kwPair.AddMember("remove", arAdd, al);
            }

            doc.AddMember(Value(i.first->formEditorID.c_str(), al), kwPair, al);
        }

        WriteJSONFile(filepath, doc);
    }

    if (bApply) ApplyKeywordChanges(params.mapKeywordChanges);

    return 0;
}

//~Keywords
//////////////////////////////////////////