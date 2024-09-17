#include "ArmorChanger.h"

#include "Config.h"
#include "Data.h"
#include "LootLists.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

using namespace rapidjson;

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
    void ReplaceRecipe(RE::BGSConstructibleObject* tar, const RE::BGSConstructibleObject* src, float w, float fCost);

    ArmorSlots RemapSlots(ArmorSlots slots, const ArmorChangeParams& params) {
        ArmorSlots slotsRemapped = slots;
        if (slots & params.remapMask) {
            for (auto s : params.mapArmorSlots) {
                if (slots &
                    (1 << s.first))  // Test on different data then the changes, so that changes aren't chained together
                    slotsRemapped = (slotsRemapped & ~(1 << s.first)) | (s.second < 32 ? (1 << s.second) : 0);
            }
            slots = slotsRemapped;
        }
        return slots;
    }

    ArmorSlots PromoteHeadSlots(ArmorSlots slots, ArmorSlots coveredHeadSlots) {
        slots &= ~kCosmeticSlotMask;

        // Promote to head slot if needed
        if (slots & coveredHeadSlots)
            slots = (slots & ~coveredHeadSlots) | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead;
        return slots;
    }

    std::pair<ArmorSlots, ArmorSlots> CalcCoveredSlots(const auto& items, const ArmorChangeParams& params,
                                                       bool remap = false) {
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

    void AddModification(const char* field, const ArmorChangeParams::SliderPair& pair, rapidjson::Value& changes,
                         MemoryPoolAllocator<>& al) {
        if (pair.bModify) changes.AddMember(StringRef(field), Value(0.01f * pair.fScale), al);
    }
}

ArmorSlots QuickArmorRebalance::GetConvertableArmorSlots(const ArmorChangeParams& params) {
    params.remapMask = 0;
    for (auto i : params.mapArmorSlots) params.remapMask |= (1 << i.first);

    auto [coveredSlots, coveredHeadSlots] = CalcCoveredSlots(params.armorSet->items, params);

    coveredSlots = 0;  // weird case where an item's in the armor set but not in the curve tree

    ProcessBaseArmorSet(params, coveredHeadSlots, [&](ArmorSlot slot, RE::TESObjectARMO*) {
        coveredSlots |= GetAllCoveredSlots(*params.curve, slot);
    });

    return coveredSlots;
}

void QuickArmorRebalance::MakeArmorChanges(const ArmorChangeParams& params) {
    if (params.items.empty()) return;
    params.bMixedSetDone = false;

    params.remapMask = 0;
    for (auto i : params.mapArmorSlots) params.remapMask |= (1 << i.first);

    // Build list of base items per slot
    auto [_discard, coveredHeadSlots] = CalcCoveredSlots(params.armorSet->items, params);
    auto [coveredSlots, coveredHeadSlotsChanges] = CalcCoveredSlots(params.items, params, true);

    SlotRelativeWeight slotValues[32];

    ProcessBaseArmorSet(params, coveredHeadSlots,
                        [&](ArmorSlot slot, RE::TESObjectARMO* i) { slotValues[slot].item = i; });

    int totalWeight = 0;
    for (const auto& i : *params.curve)
        totalWeight += GetTotalWeight(&i, ~(ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kShield);
    for (const auto& i : *params.curve) PropogateBaseValues(slotValues, nullptr, &i);
    for (const auto& i : *params.curve) CalcCoveredValues(slotValues, coveredSlots, &i);

    Document doc;
    auto& al = doc.GetAllocator();

    std::map<RE::TESFile*, Value> mapFileChanges;

    for (auto i : params.items) {
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
                // float rel = itemBase->weightBase > 0 ? (float)weight / itemBase->weightBase : 0.0f;
                Value changes(kObjectType);
                changes.AddMember("name", Value(i->GetName(), al), al);
                changes.AddMember("srcname", Value(itemBase->item->GetFullName(), al), al);
                changes.AddMember("srcfile", Value(itemBase->item->GetFile(0)->fileName, al), al);
                changes.AddMember("srcid", Value(GetFileId(itemBase->item)), al);

                // changes.AddMember("w", Value(rel), al);
                Value weights(kObjectType);
                weights.AddMember("item", weight, al);
                weights.AddMember("base", itemBase->weightBase, al);
                weights.AddMember("set", totalWeight, al);
                changes.AddMember("w", weights, al);

                if (params.bModifyKeywords) changes.AddMember("keywords", Value(true), al);

                AddModification("armor", params.armor.rating, changes, al);
                AddModification("weight", params.armor.weight, changes, al);
                AddModification("warmth", params.armor.warmth, changes, al);
                if (g_Config.isFrostfallInstalled || g_Config.bShowFrostfallCoverage)
                    changes.AddMember("coverage", Value(0.01f * params.armor.coverage), al);
                AddModification("value", params.value, changes, al);

                if (slotsRemapped != slotsOrig) changes.AddMember("slots", slotsRemapped, al);

                if (params.temper.bModify) {
                    Value recipe(kObjectType);
                    if (params.temper.bNew) recipe.AddMember("new", Value(true), al);
                    if (params.temper.bFree) recipe.AddMember("free", Value(true), al);
                    changes.AddMember("temper", recipe, al);
                }
                if (params.craft.bModify) {
                    Value recipe(kObjectType);
                    if (params.craft.bNew) recipe.AddMember("new", Value(true), al);
                    if (params.craft.bFree) recipe.AddMember("free", Value(true), al);
                    changes.AddMember("craft", recipe, al);
                }

                auto loot = MakeLootChanges(params, armor, al);
                if (!loot.IsNull()) changes.AddMember("loot", loot, al);

                Value* ls = &mapFileChanges[i->GetFile(0)];
                if (!ls->IsObject()) ls->SetObject();

                ls->AddMember(Value(std::to_string(GetFileId(i)).c_str(), al), changes, al);
            } else
                logger::debug("No item base, skipping changes to {}", i->GetName());
        } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
            if (auto itemBase = params.armorSet->FindMatching(weap)) {
                Value changes(kObjectType);
                changes.AddMember("name", Value(i->GetName(), al), al);
                changes.AddMember("srcname", Value(itemBase->GetFullName(), al), al);
                changes.AddMember("srcfile", Value(itemBase->GetFile(0)->fileName, al), al);
                changes.AddMember("srcid", Value(GetFileId(itemBase)), al);

                if (params.bModifyKeywords) changes.AddMember("keywords", Value(true), al);

                AddModification("damage", params.weapon.damage, changes, al);
                AddModification("weight", params.weapon.weight, changes, al);
                AddModification("speed", params.weapon.speed, changes, al);
                AddModification("stagger", params.weapon.stagger, changes, al);
                AddModification("value", params.value, changes, al);

                if (params.temper.bModify) {
                    Value recipe(kObjectType);
                    if (params.temper.bNew) recipe.AddMember("new", Value(true), al);
                    if (params.temper.bFree) recipe.AddMember("free", Value(true), al);
                    changes.AddMember("temper", recipe, al);
                }
                if (params.craft.bModify) {
                    Value recipe(kObjectType);
                    if (params.craft.bNew) recipe.AddMember("new", Value(true), al);
                    if (params.craft.bFree) recipe.AddMember("free", Value(true), al);
                    changes.AddMember("craft", recipe, al);
                }

                auto loot = MakeLootChanges(params, weap, al);
                if (!loot.IsNull()) changes.AddMember("loot", loot, al);

                Value* ls = &mapFileChanges[i->GetFile(0)];
                if (!ls->IsObject()) ls->SetObject();

                ls->AddMember(Value(std::to_string(GetFileId(i)).c_str(), al), changes, al);
            } else
                logger::debug("No item base, skipping changes to {}", i->GetName());
        } else if (auto ammo = i->As<RE::TESAmmo>()) {
            if (auto itemBase = params.armorSet->FindMatching(ammo)) {
                Value changes(kObjectType);
                changes.AddMember("name", Value(i->GetName(), al), al);
                changes.AddMember("srcname", Value(itemBase->GetFullName(), al), al);
                changes.AddMember("srcfile", Value(itemBase->GetFile(0)->fileName, al), al);
                changes.AddMember("srcid", Value(GetFileId(itemBase)), al);

                if (params.bModifyKeywords) changes.AddMember("keywords", Value(true), al);

                AddModification("damage", params.weapon.damage, changes, al);
                AddModification("weight", params.weapon.weight, changes, al);
                AddModification("value", params.value, changes, al);

                if (params.temper.bModify) {
                    Value recipe(kObjectType);
                    if (params.temper.bNew) recipe.AddMember("new", Value(true), al);
                    if (params.temper.bFree) recipe.AddMember("free", Value(true), al);
                    changes.AddMember("temper", recipe, al);
                }
                if (params.craft.bModify) {
                    Value recipe(kObjectType);
                    if (params.craft.bNew) recipe.AddMember("new", Value(true), al);
                    if (params.craft.bFree) recipe.AddMember("free", Value(true), al);
                    changes.AddMember("craft", recipe, al);
                }

                auto loot = MakeLootChanges(params, ammo, al);
                if (!loot.IsNull()) changes.AddMember("loot", loot, al);

                Value* ls = &mapFileChanges[i->GetFile(0)];
                if (!ls->IsObject()) ls->SetObject();

                ls->AddMember(Value(std::to_string(GetFileId(i)).c_str(), al), changes, al);
            } else
                logger::debug("No item base, skipping changes to {}", i->GetName());
        }
    }

    for (auto& i : mapFileChanges) {
        ApplyChanges(i.first, i.second, g_Config.permLocal);

        std::filesystem::path path(std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/");
        std::filesystem::create_directories(path);

        path /= i.first->fileName;
        path += ".json";

        if (std::filesystem::exists(path.generic_string().c_str())) {
            if (auto fp = std::fopen(path.generic_string().c_str(), "rb")) {
                char readBuffer[1 << 16];
                FileReadStream is(fp, readBuffer, sizeof(readBuffer));
                doc.ParseStream(is);
                std::fclose(fp);

                if (doc.HasParseError()) {
                    logger::warn("{}: JSON parse error: {} ({})", path.generic_string(),
                                 GetParseError_En(doc.GetParseError()), doc.GetErrorOffset());
                    logger::warn("{}: Overwriting previous file contents due to parsing error", path.generic_string());
                    doc.SetObject();
                }

                if (!doc.IsObject()) {
                    logger::warn("{}: Unexpected contents, overwriting previous contents", path.generic_string());
                    doc.SetObject();
                }

            } else {
                logger::warn("Could not open file {}", path.filename().generic_string());
                continue;
            }
        } else {
            doc.SetObject();
        }

        if (!i.second.IsObject()) continue;

        for (auto& j : i.second.GetObj()) {
            if (!params.bMerge) {
                doc.RemoveMember(j.name);  // it doesn't automaticaly remove duplicates
                doc.AddMember(j.name, j.value, al);
            } else {
                if (doc.HasMember(j.name)) {
                    auto& prev = doc[j.name];
                    if (!prev.IsObject()) {
                        doc.RemoveMember(j.name);
                        doc.AddMember(j.name, j.value, al);
                        continue;
                    }

                    for (auto& m : j.value.GetObj()) {
                        prev.RemoveMember(m.name);
                        prev.AddMember(m.name, m.value, al);
                    }
                } else
                    doc.AddMember(j.name, j.value, al);
            }
        }

        if (auto fp = std::fopen(path.generic_string().c_str(), "wb")) {
            char buffer[1 << 16];
            FileWriteStream ws(fp, buffer, sizeof(buffer));
            PrettyWriter<FileWriteStream> writer(ws);
            writer.SetIndent('\t', 1);
            doc.Accept(writer);
            std::fclose(fp);
        } else {
            logger::error("Could not open file to write {}: {}", path.generic_string(), std::strerror(errno));

            g_Config.strCriticalError = std::format(
                "Unable to write to file {}\n"
                "Path: {}\n"
                "Error: {}\n"
                "Changes cannot be saved so further use of QAR is disabled",
                path.filename().generic_string(), path.generic_string(), std::strerror(errno));
        }
    }
}

void QuickArmorRebalance::ApplyChanges(const RE::TESFile* file, const rapidjson::Value& ls, const Permissions& perm) {
    if (!ls.IsObject()) return;

    int nChanges = 0;

    for (auto& i : ls.GetObj()) {
        if (!i.name.IsString()) {
            logger::error("Invalid item id in {}", file->fileName);
            continue;
        }

        RE::FormID id = atoi(i.name.GetString());
        if (ApplyChanges(file, id, i.value, perm))
            nChanges++;
        else
            logger::error("Failed to apply changes to {}:{:#08x}", file->fileName, id);
    }

    logger::info("{}: {} changes made", file->fileName, nChanges);

    if (nChanges > 0) {
        if (&perm == &g_Config.permShared) {
            g_Data.modifiedFilesShared.insert(file);
        } else {
            g_Data.modifiedFiles.insert(file);
            g_Data.modifiedFilesDeleted.erase(file);
        }
    }
}

template <class T, typename V>
bool ChangeField(bool bAllowed, const char* field, const rapidjson::Value& changes, T* src, T* item, V T::*member,
                 const auto& fn) {
    if (bAllowed && changes.HasMember(field)) {
        auto& jsonScale = changes[field];
        if (!jsonScale.IsFloat()) return false;

        auto scale = jsonScale.GetFloat();
        if (src->*member && scale > 0.0f)
            item->*member = fn(scale * src->*member);
        else
            item->*member = 0;
    }

    return true;
}

void GetMatchingKeywords(const std::set<RE::BGSKeyword*>& set, std::vector<RE::BGSKeyword*>& addKwds,
                         const RE::BGSKeywordForm* src) {
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

bool QuickArmorRebalance::ApplyChanges(const RE::TESFile* file, RE::FormID id, const rapidjson::Value& changes,
                                       const Permissions& perm) {
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

    float weight = 1.0f;

    if (&perm == &g_Config.permShared)
        g_Data.modifiedItemsShared.insert(bo);
    else
        g_Data.modifiedItems.insert(bo);

    if (auto armor = item->As<RE::TESObjectARMO>()) {
        auto src = objSrc->As<RE::TESObjectARMO>();
        if (!src) return true;

        if (!changes.HasMember("w")) return false;
        auto& jsonW = changes["w"];

        int itemW = 0;
        int baseW = 0;
        int setW = 0;

        if (jsonW.IsFloat())
            weight = jsonW.GetFloat();
        else if (jsonW.IsObject()) {
            auto wObj = jsonW.GetObj();

            itemW = wObj["item"].GetInt();
            baseW = wObj["base"].GetInt();
            setW = wObj["set"].GetInt();

            weight = baseW > 0 ? (float)itemW / baseW : 0.0f;
        } else
            return false;

        if (!ChangeField<RE::TESObjectARMO>(perm.bModifyArmorRating, "armor", changes, src, armor,
                                            &RE::TESObjectARMO::armorRating,
                                            [=](float f) { return std::max(1, (int)(weight * f)); }))
            return false;

        if (!ChangeField<RE::TESObjectARMO>(perm.bModifyWeight, "weight", changes, src, armor,
                                            &RE::TESObjectARMO::weight, [=](float f) {
                                                f *= weight;
                                                if (g_Config.bRoundWeight)
                                                    f = std::max(0.1f, 0.1f * std::round(10.0f * f));
                                                return f;
                                            }))
            return false;

        if (!ChangeField<RE::TESObjectARMO>(perm.bModifyValue, "value", changes, src, armor, &RE::TESObjectARMO::value,
                                            [=](float f) { return std::max(1, (int)(weight * f)); }))
            return false;

        if (perm.bModifySlots && changes.HasMember("slots")) {
            auto& jsonOption = changes["slots"];
            if (!jsonOption.IsUint()) return false;

            if (!g_Data.modifiedArmorSlots.contains(armor))  // Don't overwrite previous
                g_Data.modifiedArmorSlots[armor] = armor->bipedModelData.bipedObjectSlots.underlying();

            auto slots = (RE::BIPED_MODEL::BipedObjectSlot)jsonOption.GetUint();
            armor->bipedModelData.bipedObjectSlots = slots;
            for (auto addon : armor->armorAddons) {
                if (addon->GetFile(0) == armor->GetFile(0)) {  // Don't match other files, might be placeholders
                    addon->bipedModelData.bipedObjectSlots = slots;
                    
                    for (int i = 0; i < RE::SEXES::kTotal; i++) {
                        if (!addon->bipedModels[i].model.empty())
                        {
                            std::string modelPath(addon->bipedModels[i].model);
                            std::transform(modelPath.begin(), modelPath.end(), modelPath.begin(), ::tolower);

                            auto hash = std::hash<std::string>{}(modelPath);
                            if (!g_Data.noModifyModels.contains(hash))
                                g_Data.remapFileArmorSlots[hash] = (ArmorSlots)slots;

                            if (modelPath.length() > 6) {
                                char* pChar = modelPath.data() + modelPath.length() - 6;  //'_X.nif'
                                if (*pChar++ == '_') {
                                    if (*pChar == '0') {
                                        *pChar = '1';
                                        hash = std::hash<std::string>{}(modelPath);
                                        if (!g_Data.noModifyModels.contains(hash))
                                            g_Data.remapFileArmorSlots[hash] = (ArmorSlots)slots;
                                    }
                                    else if (*pChar == '1') {
                                        *pChar = '0';
                                        hash = std::hash<std::string>{}(modelPath);
                                        if (!g_Data.noModifyModels.contains(hash))
                                            g_Data.remapFileArmorSlots[hash] = (ArmorSlots)slots;
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
            if (jsonScale.IsFloat()) {
                auto scale = warmth = jsonScale.GetFloat();
                g_Data.modifiedWarmth[armor] = scale * itemW / setW * 150.0f;
            }

            if (g_Config.isFrostfallInstalled && changes.HasMember("coverage")) {
                auto coverage = changes["coverage"].GetFloat();
                bModifyFFKeywords = true;

                constexpr auto ffSlots = (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kBody |
                                         (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead |
                                         (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kFeet |
                                         (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHands;

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
                armor->bipedModelData.armorType = src->bipedModelData.armorType;

                GetMatchingKeywords(g_Config.kwSet, addKwds, src);

                if ((unsigned int)src->GetSlotMask() & (unsigned int)armor->GetSlotMask()) {
                    GetMatchingKeywords(g_Config.kwSlotSpecSet, addKwds, src);
                }

                MatchKeywords(armor, addKwds, [=](RE::BGSKeyword* kw) {
                    return g_Config.kwSet.contains(kw) || g_Config.kwSlotSpecSet.contains(kw) ||
                           (bModifyFFKeywords && g_Config.kwFFSet.contains(kw));
                });
            }
        }
    } else if (auto weap = item->As<RE::TESObjectWEAP>()) {
        auto src = objSrc->As<RE::TESObjectWEAP>();
        if (!src) return true;

        if (!ChangeField<RE::TESObjectWEAP>(perm.bModifyWeapDamage, "damage", changes, src, weap,
                                            &RE::TESObjectWEAP::attackDamage,
                                            [=](float f) { return (uint16_t)std::max(1, (int)f); }))
            return false;

        if (!ChangeField<RE::TESObjectWEAP::CriticalData>(perm.bModifyWeapDamage, "damage", changes, &src->criticalData,
                                                          &weap->criticalData, &RE::TESObjectWEAP::CriticalData::damage,
                                                          [=](float f) { return (uint16_t)std::max(1, (int)f); }))
            return false;
        if (perm.bModifyWeapDamage) weap->criticalData.prcntMult = src->criticalData.prcntMult;

        if (!ChangeField<RE::TESObjectWEAP>(perm.bModifyWeapWeight, "weight", changes, src, weap,
                                            &RE::TESObjectWEAP::weight, [=](float f) {
                                                f *= weight;
                                                if (g_Config.bRoundWeight)
                                                    f = std::max(0.1f, 0.1f * std::round(10.0f * f));
                                                return f;
                                            }))
            return false;

        if (!ChangeField<RE::TESObjectWEAP::Data>(perm.bModifyWeapSpeed, "speed", changes, &src->weaponData,
                                                  &weap->weaponData, &RE::TESObjectWEAP::Data::speed,
                                                  [=](float f) { return f; }))
            return false;

        if (!ChangeField<RE::TESObjectWEAP::Data>(perm.bModifyWeapStagger, "stagger", changes, &src->weaponData,
                                                  &weap->weaponData, &RE::TESObjectWEAP::Data::staggerValue,
                                                  [=](float f) { return f; }))
            return false;

        if (!ChangeField<RE::TESObjectWEAP>(perm.bModifyValue, "value", changes, src, weap, &RE::TESObjectWEAP::value,
                                            [=](float f) { return std::max(1, (int)f); }))
            return false;

        if (perm.bModifyKeywords && changes.HasMember("keywords")) {
            auto& jsonOption = changes["keywords"];
            if (!jsonOption.IsBool()) return false;

            if (jsonOption.GetBool()) {
                logger::trace("Changing keywords");

                std::vector<RE::BGSKeyword*> addKwds;

                GetMatchingKeywords(g_Config.kwSetWeap, addKwds, src);
                MatchKeywords(weap, addKwds, [](RE::BGSKeyword* kw) { return g_Config.kwSetWeap.contains(kw); });
            }
        }
    } else if (auto ammo = item->As<RE::TESAmmo>()) {
        auto src = objSrc->As<RE::TESAmmo>();
        if (!src) return true;

        if (!ChangeField<RE::AMMO_DATA>(perm.bModifyWeapDamage, "damage", changes, &src->GetRuntimeData().data,
                                        &ammo->GetRuntimeData().data, &RE::AMMO_DATA::damage,
                                        [=](float f) { return std::max(1.0f, f); }))
            return false;

        if (!ChangeField<RE::TESAmmo>(perm.bModifyValue, "value", changes, src, ammo, &RE::TESAmmo::value,
                                      [=](float f) { return std::max(1, (int)f); }))
            return false;

        if (perm.bModifyKeywords && changes.HasMember("keywords")) {
            auto& jsonOption = changes["keywords"];
            if (!jsonOption.IsBool()) return false;

            if (jsonOption.GetBool()) {
                logger::trace("Changing keywords");

                std::vector<RE::BGSKeyword*> addKwds;

                GetMatchingKeywords(g_Config.kwSetWeap, addKwds, src->AsKeywordForm());
                MatchKeywords(ammo->AsKeywordForm(), addKwds,
                              [](RE::BGSKeyword* kw) { return g_Config.kwSetWeap.contains(kw); });
            }
        }
    }

    int rarity = -1;
    if (perm.bDistributeLoot) {
        if (changes.HasMember("loot")) {
            auto& jsonLoot = changes["loot"];
            if (!jsonLoot.IsObject()) return false;

            if (jsonLoot.HasMember("rarity") && jsonLoot["rarity"].IsInt()) rarity = jsonLoot["rarity"].GetInt();

            if (g_Data.loot) LoadLootChanges(bo, jsonLoot);
        }
    }

    if (perm.temper.bModify && changes.HasMember("temper") && !item->As<RE::TESAmmo>()) {
        auto& jsonOpts = changes["temper"];
        if (!jsonOpts.IsObject()) return false;

        auto opts = jsonOpts.GetObj();

        bool bNew = false;
        bool bFree = false;

        if (opts.HasMember("new")) bNew = opts["new"].GetBool();
        if (opts.HasMember("free")) bFree = opts["free"].GetBool();

        RE::BGSConstructibleObject *recipeItem = nullptr, *recipeSrc = nullptr;

        auto it = g_Data.temperRecipe.find(bo);
        if (it != g_Data.temperRecipe.end()) recipeItem = it->second;

        it = g_Data.temperRecipe.find(boSrc);
        if (it != g_Data.temperRecipe.end()) recipeSrc = it->second;

        bFree = (bFree && perm.crafting.bFree) || recipeSrc;  // If recipe source, free doesn't matter

        if (!recipeItem && bNew && perm.temper.bCreate && bFree) {
            auto newForm = static_cast<RE::BGSConstructibleObject*>(
                RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

            if (newForm) {
                newForm->benchKeyword =
                    recipeSrc
                        ? recipeSrc->benchKeyword
                        : (item->As<RE::TESObjectARMO>()
                               ? RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingArmorTable")
                               : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingSharpeningWheel"));
                newForm->createdItem = item;
                newForm->data.numConstructed = 1;

                dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(
                    newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                g_Data.temperRecipe.insert({bo, recipeItem});
                recipeItem = newForm;
            }
        }

        if (recipeItem && bFree) ::ReplaceRecipe(recipeItem, recipeSrc, weight, g_Config.fTemperGoldCostRatio);
    }

    if (perm.crafting.bModify && changes.HasMember("craft")) {
        auto& jsonOpts = changes["craft"];
        if (!jsonOpts.IsObject()) return false;

        auto opts = jsonOpts.GetObj();

        bool bNew = false;
        bool bFree = false;

        if (opts.HasMember("new")) bNew = opts["new"].GetBool();
        if (opts.HasMember("free")) bFree = opts["free"].GetBool();

        RE::BGSConstructibleObject *recipeItem = nullptr, *recipeSrc = nullptr;

        auto it = g_Data.craftRecipe.find(bo);
        if (it != g_Data.craftRecipe.end()) recipeItem = it->second;

        it = g_Data.craftRecipe.find(boSrc);
        if (it != g_Data.craftRecipe.end()) recipeSrc = it->second;

        bFree = (bFree && perm.crafting.bFree) || recipeSrc;  // If recipe source, free doesn't matter

        if (rarity <= g_Config.craftingRarityMax) {
            if (!recipeItem && bNew && perm.crafting.bCreate && bFree) {
                auto newForm = static_cast<RE::BGSConstructibleObject*>(
                    RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

                if (newForm) {
                    newForm->benchKeyword =
                        recipeSrc ? recipeSrc->benchKeyword
                                  : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingForge");
                    newForm->createdItem = item;
                    newForm->data.numConstructed = item->As<RE::TESAmmo>() ? 24 : 1;

                    dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(
                        newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                    g_Data.craftRecipe.insert({bo, recipeItem});
                    recipeItem = newForm;
                }
            }

            if (recipeItem && bFree) ::ReplaceRecipe(recipeItem, recipeSrc, weight, g_Config.fCraftGoldCostRatio);
        } else if (g_Config.bDisableCraftingRecipesOnRarity && recipeItem) {
            logger::trace("Disabling recipe for {}", item->GetName());
            recipeItem->benchKeyword = nullptr;
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
                auto newForm = static_cast<RE::BGSConstructibleObject*>(
                    RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

                if (newForm) {
                    newForm->benchKeyword = recipeSrc
                                                ? recipeSrc->benchKeyword
                                                : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmelter");

                    dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(
                        newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                    g_Data.smeltRecipe.insert({bo, recipeItem});
                    recipeItem = newForm;
                }

            } else
                ::ClearRecipe(recipeItem);

            if (recipeItem) {
                ::CopyConditions(recipeItem->conditions, recipeSrc->conditions,
                                 recipeSrc->requiredItems.containerObjects[0]->obj, bo);

                auto& mats = recipeItem->requiredItems;
                mats.containerObjects = RE::calloc<RE::ContainerObject*>(mats.numContainerObjects = 1);
                mats.containerObjects[0] =
                    new RE::ContainerObject(bo, recipeSrc->requiredItems.containerObjects[0]->count);

                recipeItem->data.numConstructed = (uint16_t)std::max(1, (int)(weight * recipeSrc->data.numConstructed));
                recipeItem->createdItem = recipeSrc->createdItem;
            }
        }
    }

    return true;
}

void ::CopyConditions(RE::TESCondition& tar, const RE::TESCondition& src, void* replace, void* replaceWith) {
    auto prev = tar.head;
    if (prev)
        while (prev->next) prev = prev->next;

    if (src.head) {
        for (auto psrc = src.head; psrc; psrc = psrc->next) {
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

void ::ClearRecipe(RE::BGSConstructibleObject* tar) {
    {  // Materials
        auto& mats = tar->requiredItems;

        if (mats.containerObjects) {
            for (unsigned int i = 0; i < mats.numContainerObjects; i++) delete mats.containerObjects[i];
            RE::free(mats.containerObjects);
        }

        mats.containerObjects = nullptr;
        mats.numContainerObjects = 0;
    }

    {  // Conditions
        RE::TESConditionItem* book = nullptr;
        auto& conds = tar->conditions;

        for (auto head = conds.head; head;) {
            auto next = head->next;

            if (QuickArmorRebalance::g_Config.bKeepCraftingBooks &&
                head->data.functionData.function == RE::FUNCTION_DATA::FunctionID::kGetItemCount) {
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

void ::ReplaceRecipe(RE::BGSConstructibleObject* tar, const RE::BGSConstructibleObject* src, float w, float fCost) {
    ::ClearRecipe(tar);
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
                    mats.containerObjects[i] = new RE::ContainerObject(
                        srcmats.containerObjects[i]->obj, std::max(1, (int)(w * srcmats.containerObjects[i]->count)));
                }
            }
        }

        ::CopyConditions(tar->conditions, src->conditions, src->createdItem, tar->createdItem);
    } else if (fCost > 0) {
        auto& mats = tar->requiredItems;

        auto goldForm = RE::TESForm::LookupByID(0xf);
        if (!goldForm) return;

        auto goldObj = goldForm->As<RE::TESBoundObject>();
        if (!goldObj) return;

        mats.containerObjects = RE::calloc<RE::ContainerObject*>(mats.numContainerObjects = 1);
        mats.containerObjects[0] =
            new RE::ContainerObject(goldObj, std::max(1, (int)(0.01f * fCost * tar->createdItem->GetGoldValue())));
    }
}
