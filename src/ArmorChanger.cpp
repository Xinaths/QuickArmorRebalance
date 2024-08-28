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
    int GetTotalWeight(QuickArmorRebalance::RebalanceCurveNode const* node) {
        int w = node->weight;
        for (auto& i : node->children) w += GetTotalWeight(&i);
        return w;
    }

    struct SlotRelativeWeight {
        SlotRelativeWeight* base = nullptr;
        RE::TESObjectARMO* item = nullptr;
        int weightBase = 0;
        int weightUsed = 0;
    };

    int PropogateBaseValues(SlotRelativeWeight* values, SlotRelativeWeight* base,
                            QuickArmorRebalance::RebalanceCurveNode const* node) {
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

    int CalcCoveredValues(SlotRelativeWeight* values, unsigned int coveredSlots,
                          QuickArmorRebalance::RebalanceCurveNode const* node) {
        auto& v = values[node->slot - 30];
        auto w = node->weight;

        for (auto& i : node->children) w += CalcCoveredValues(values, coveredSlots, &i);

        if (coveredSlots & (1 << (node->slot - 30))) {
            v.weightUsed = w;
            return 0;
        } else
            return w;
    }

    void ClearRecipe(RE::BGSConstructibleObject* tar);
    void ReplaceRecipe(RE::BGSConstructibleObject* tar, const RE::BGSConstructibleObject* src, float w);
}

void QuickArmorRebalance::MakeArmorChanges(const ArmorChangeParams& params) {
    if (params.items.empty()) return;
    params.bMixedSetDone = false;

    unsigned int coveredSlots = 0;
    SlotRelativeWeight slotValues[32];

    // Build list of slots that will be used for scaling
    for (auto i : params.items) {
        coveredSlots |= (unsigned int)i->GetSlotMask();
    }

    // Build list of base items per slot

    //Head slot is weird and needs special handling
    unsigned int coveredHeadSlots = 0;
    for (auto i : params.armorSet->items) {
        coveredHeadSlots |= kHeadSlotMask & (unsigned int)i->GetSlotMask();
    }
    if (coveredHeadSlots & (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHead)
        coveredHeadSlots = 0; //If we have a head slot, don't need to do anything
    else //Pick the best slot only to promote
    {
        coveredHeadSlots &= (coveredHeadSlots ^ (coveredHeadSlots - 1)); //Isolate lowest bit
    }

    for (auto i : params.armorSet->items) {
        unsigned int slots = (unsigned int)i->GetSlotMask();
        slots &= ~kCosmeticSlotMask;

        //Promote to head slot if needed
        if (slots & coveredHeadSlots) slots = (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHead;

        // Handling multi-slot items makes things a giant mess, just use the lowest one instead
        // while (slots) {  // hair can cause multiple
        if (slots) {
            unsigned long slot;
            _BitScanForward(&slot, slots);
            slots &= slots - 1; //Removes lowest bit


            slotValues[slot].item = i;
        }
    }

    int totalWeight = 0;
    for (const auto& i : *params.curve) totalWeight += GetTotalWeight(&i);
    for (const auto& i : *params.curve) PropogateBaseValues(slotValues, nullptr, &i);
    for (const auto& i : *params.curve) CalcCoveredValues(slotValues, coveredSlots, &i);

    Document doc;
    auto& al = doc.GetAllocator();

    std::map<RE::TESFile*, Value> mapFileChanges;

    for (auto i : params.items) {
        SlotRelativeWeight* itemBase = nullptr;
        int weight = 0;

        unsigned int slots = (unsigned int)i->GetSlotMask();

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
        asdf
        if (itemBase) {
            float rel = itemBase->weightBase > 0 ? (float)weight / itemBase->weightBase : 0.0f;
            Value changes(kObjectType);
            changes.AddMember("name", Value(i->GetFullName(), al), al);
            changes.AddMember("srcname", Value(itemBase->item->GetFullName(), al), al);
            changes.AddMember("srcfile", Value(itemBase->item->GetFile(0)->fileName, al), al);
            changes.AddMember("srcid", Value(GetFileId(itemBase->item)), al);
            changes.AddMember("w", Value(rel), al);
            if (params.bModifyKeywords) changes.AddMember("keywords", Value(true), al);
            if (params.bModifyArmor) changes.AddMember("armor", Value(0.01f * params.fArmorScale), al);
            if (params.bModifyWeight) changes.AddMember("weight", Value(0.01f * params.fWeightScale), al);
            if (params.bModifyValue) changes.AddMember("value", Value(0.01f * params.fValueScale), al);
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

            auto loot = MakeLootChanges(params, i, al);
            if (!loot.IsNull()) changes.AddMember("loot", loot, al);

            /*
            if (!ApplyChanges(i->GetFile(0), GetFileId(i), changes, g_Config.permLocal)) {
                logger::error("Failed to apply changes");
            }
            */

            Value* ls = &mapFileChanges[i->GetFile(0)];
            if (!ls->IsObject()) ls->SetObject();

            ls->AddMember(Value(std::to_string(GetFileId(i)).c_str(), al), changes, al);
        } else
            logger::error("No item base, skipping changes to {}", i->fullName);
    }

    for (auto& i : mapFileChanges) {
        ApplyChanges(i.first, i.second, g_Config.permLocal);

        std::filesystem::path path(std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/");
        path /= i.first->fileName;
        path += ".json";

        if (std::filesystem::exists(path.generic_string().c_str())) {
            if (auto fp = std::fopen(path.generic_string().c_str(), "rb")) {
                char readBuffer[1 << 16];
                FileReadStream is(fp, readBuffer, sizeof(readBuffer));
                doc.ParseStream(is);
                std::fclose(fp);

                if (doc.HasParseError()) {
                    logger::warn("{}: JSON parse error: {} ({})",
                                 path.generic_string(), GetParseError_En(doc.GetParseError()), doc.GetErrorOffset());
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
        } else
            doc.SetObject();

        if (!i.second.IsObject()) continue;

        for (auto& j : i.second.GetObj()) {
            doc.RemoveMember(j.name); //it doesn't automaticaly remove duplicates
            doc.AddMember(j.name, j.value, al);
        }

        if (auto fp = std::fopen(path.generic_string().c_str(), "wb")) {
            char buffer[1 << 16];
            FileWriteStream ws(fp, buffer, sizeof(buffer));
            PrettyWriter<FileWriteStream> writer(ws);
            writer.SetIndent('\t', 1);
            doc.Accept(writer);
            std::fclose(fp);
        } else
            logger::error("Could not open file to write {}", path.filename().generic_string());
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

bool QuickArmorRebalance::ApplyChanges(const RE::TESFile* file, RE::FormID id, const rapidjson::Value& changes,
                                       const Permissions& perm) {
    if (!changes.IsObject()) return false;

    auto dataHandler = RE::TESDataHandler::GetSingleton();

    if (!changes.HasMember("srcfile") || !changes.HasMember("srcid")) return false;

    auto& jsonSrcFile = changes["srcfile"];
    auto& jsonSrcId = changes["srcid"];

    if (!jsonSrcFile.IsString() || !jsonSrcId.IsUint()) return false;

    auto src = dataHandler->LookupForm<RE::TESObjectARMO>(jsonSrcId.GetUint(), jsonSrcFile.GetString());
    if (!src) return true;

    auto item = RE::TESForm::LookupByID<RE::TESObjectARMO>(GetFullId(file, id));
    if (!item) {
        logger::info("Item not found {}:{:#08x}", file->fileName, id);
        return true;
    }

    if (!changes.HasMember("w")) return false;
    auto& jsonW = changes["w"];

    if (!jsonW.IsFloat()) return false;
    float weight = jsonW.GetFloat();

    if (&perm == &g_Config.permShared) g_Data.modifiedItemsShared.insert(item);
    else g_Data.modifiedItems.insert(item);

    if (perm.bModifyArmorRating && changes.HasMember("armor")) {
        auto& jsonScale = changes["armor"];
        if (!jsonScale.IsFloat()) return false;

        logger::trace("Changing armor rating");

        auto scale = jsonScale.GetFloat();
        if (src->armorRating && scale > 0.0f)
            item->armorRating = std::max(1, (int)(weight * scale * src->armorRating));
        else
            item->armorRating = 0;
    }

    if (perm.bModifyWeight && changes.HasMember("weight")) {
        auto& jsonScale = changes["weight"];
        if (!jsonScale.IsFloat()) return false;

        logger::trace("Changing weight");

        auto scale = jsonScale.GetFloat();
        if (scale > 0.0f) {
            item->weight = weight * scale * src->weight;
            if (g_Config.bRoundWeight) item->weight = std::max(0.1f, 0.1f * std::round(10.0f * item->weight));
        }
        else
            item->weight = 0;
    }

    if (perm.bModifyWeight && changes.HasMember("value")) {
        auto& jsonScale = changes["value"];
        if (!jsonScale.IsFloat()) return false;

        logger::trace("Changing value");

        auto scale = jsonScale.GetFloat();
        if (src->value > 0 && scale > 0.0f)
            item->value = std::max(1, (int)(weight * scale * src->value));
        else
            item->value = 0;
    }

    if (perm.bModifyKeywords && changes.HasMember("keywords")) {
        auto& jsonOption = changes["keywords"];
        if (!jsonOption.IsBool()) return false;

        if (jsonOption.GetBool()) {
            logger::trace("Changing keywords");
            item->bipedModelData.armorType = src->bipedModelData.armorType;

            std::vector<RE::BGSKeyword*> addKwds, delKwds;

            for (unsigned int i = 0; i < src->numKeywords; i++) {
                if (g_Config.kwSet.contains(src->keywords[i])) addKwds.push_back(src->keywords[i]);
            }

            if ((unsigned int)src->GetSlotMask() & (unsigned int)item->GetSlotMask())
            {
                for (unsigned int i = 0; i < src->numKeywords; i++) {
                    if (g_Config.kwSlotSpecSet.contains(src->keywords[i])) addKwds.push_back(src->keywords[i]);
                }
            }

            for (unsigned int i = 0; i < item->numKeywords; i++) {
                if (g_Config.kwSet.contains(item->keywords[i]) || g_Config.kwSlotSpecSet.contains(item->keywords[i])) {
                    if (addKwds.empty())
                        delKwds.push_back(item->keywords[i]);
                    else {
                        item->keywords[i] = addKwds.back();
                        addKwds.pop_back();
                    }
                }
            }

            // A more efficient bulk add/remove function exists, but it's not showing up for me and I currently don't
            // feel like spending the time to figure out why
            while (!addKwds.empty()) {
                item->AddKeyword(addKwds.back());
                addKwds.pop_back();
            }

            while (!delKwds.empty()) {
                item->RemoveKeyword(delKwds.back());
                delKwds.pop_back();
            }
        }
    }

    int rarity = -1;
    if (perm.bDistributeLoot) {
        if (changes.HasMember("loot")) {
            auto& jsonLoot = changes["loot"];
            if (!jsonLoot.IsObject()) return false;

            if (jsonLoot.HasMember("rarity") && jsonLoot["rarity"].IsInt()) rarity = jsonLoot["rarity"].GetInt();

            if (g_Data.loot)
                LoadLootChanges(item, jsonLoot);
        }
    }

    if (perm.temper.bModify && changes.HasMember("temper")) {
        auto& jsonOpts = changes["temper"];
        if (!jsonOpts.IsObject()) return false;

        auto opts = jsonOpts.GetObj();

        bool bNew = false;
        bool bFree = false;

        if (opts.HasMember("new")) bNew = opts["new"].GetBool();
        if (opts.HasMember("free")) bFree = opts["free"].GetBool();

        RE::BGSConstructibleObject *recipeItem = nullptr, *recipeSrc = nullptr;

        auto it = g_Data.temperRecipe.find(item);
        if (it != g_Data.temperRecipe.end()) recipeItem = it->second;

        it = g_Data.temperRecipe.find(src);
        if (it != g_Data.temperRecipe.end()) recipeSrc = it->second;

        bFree = (bFree && perm.crafting.bFree) || recipeSrc;  // If recipe source, free doesn't matter

        if (!recipeItem && bNew && perm.temper.bCreate && bFree) {
            auto newForm = static_cast<RE::BGSConstructibleObject*>(
                RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

            if (newForm) {
                newForm->benchKeyword =
                    recipeSrc ? recipeSrc->benchKeyword
                              : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingArmorTable");
                newForm->createdItem = item;
                newForm->data.numConstructed = 1;

                dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(
                    newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                g_Data.temperRecipe.insert({item, recipeItem});
                recipeItem = newForm;
            }
        }

        if (recipeItem && bFree) ::ReplaceRecipe(recipeItem, recipeSrc, weight);
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

        auto it = g_Data.craftRecipe.find(item);
        if (it != g_Data.craftRecipe.end()) recipeItem = it->second;

        it = g_Data.craftRecipe.find(src);
        if (it != g_Data.craftRecipe.end()) recipeSrc = it->second;

        bFree = (bFree && perm.crafting.bFree) || recipeSrc; //If recipe source, free doesn't matter

        if (rarity <= g_Config.craftingRarityMax) {
            if (!recipeItem && bNew && perm.crafting.bCreate && bFree) {
                auto newForm = static_cast<RE::BGSConstructibleObject*>(
                    RE::IFormFactory::GetFormFactoryByType(RE::FormType::ConstructibleObject)->Create());

                if (newForm) {
                    newForm->benchKeyword =
                        recipeSrc ? recipeSrc->benchKeyword
                                  : RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingForge");
                    newForm->createdItem = item;
                    newForm->data.numConstructed = 1;

                    dataHandler->GetFormArray<RE::BGSConstructibleObject>().push_back(
                        newForm);  // For whatever reason, it's not added automatically and thus won't show up in game
                    g_Data.craftRecipe.insert({item, recipeItem});
                    recipeItem = newForm;
                }
            }

            if (recipeItem && bFree) ::ReplaceRecipe(recipeItem, recipeSrc, weight);
        } else if (g_Config.bDisableCraftingRecipesOnRarity && recipeItem)
        {
            logger::trace("Disabling recipe for {}", item->fullName);
            recipeItem->benchKeyword = nullptr;
        }
    }


    return true;
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

            if (QuickArmorRebalance::g_Config.bKeepCraftingBooks && head->data.functionData.function == RE::FUNCTION_DATA::FunctionID::kGetItemCount)
            {
                head->next = book;
                book = head;
            } else
                delete head;

            head = next;
        }
        conds.head = book;
    }
}

void ::ReplaceRecipe(RE::BGSConstructibleObject* tar, const RE::BGSConstructibleObject* src, float w) {
    ::ClearRecipe(tar);
    if (!src) return;

    tar->benchKeyword = src->benchKeyword;

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
    {  // Conditions
        const auto& srcConds = src->conditions;
        auto& conds = tar->conditions;

        if (srcConds.head) {
            for (auto psrc = srcConds.head; psrc; psrc = psrc->next) {
                auto p = new RE::TESConditionItem();
                p->data = psrc->data;
                p->next = conds.head;
                conds.head = p;
            }
        }
    }
}
