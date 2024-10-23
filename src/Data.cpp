#include "Data.h"

#include "ArmorChanger.h"
#include "Config.h"
#include "ModIntegrations.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

using namespace rapidjson;

namespace QuickArmorRebalance {
    void LoadChangesFromFolder(const char* sub, const Permissions& perm);
    bool LoadFileChanges(const RE::TESFile* mod, std::filesystem::path path, const Permissions& perm);
}

using namespace QuickArmorRebalance;

ProcessedData QuickArmorRebalance::g_Data;

bool QuickArmorRebalance::ReadJSONFile(std::filesystem::path path, Document& doc, bool bEditing) {
    if (std::filesystem::exists(path)) {
        if (auto fp = std::fopen(path.generic_string().c_str(), "rb")) {
            char readBuffer[1 << 16];
            FileReadStream is(fp, readBuffer, sizeof(readBuffer));
            doc.ParseStream<kParseCommentsFlag | kParseTrailingCommasFlag>(is);
            std::fclose(fp);

            if (doc.HasParseError()) {
                logger::warn("{}: JSON parse error: {} ({})", path.generic_string(),
                             GetParseError_En(doc.GetParseError()), doc.GetErrorOffset());
                if (bEditing) {
                    logger::warn("{}: Overwriting previous file contents due to parsing error", path.generic_string());
                    doc.SetObject();
                }
            }

            if (!doc.IsObject()) {
                if (bEditing) {
                    logger::warn("{}: Unexpected contents, overwriting previous contents", path.generic_string());
                    doc.SetObject();
                }
            }

        } else {
            logger::warn("Could not open file {}", path.filename().generic_string());
            return false;
        }
    } else {
        doc.SetObject();
    }

    return true;
}

bool QuickArmorRebalance::WriteJSONFile(std::filesystem::path path, rapidjson::Document& doc) {
    if (auto fp = std::fopen(path.generic_string().c_str(), "wb")) {
        char buffer[1 << 16];
        FileWriteStream ws(fp, buffer, sizeof(buffer));
        PrettyWriter<FileWriteStream> writer(ws);
        writer.SetIndent('\t', 1);
        doc.Accept(writer);
        std::fclose(fp);
        return true;
    } else {
        logger::error("Could not open file to write {}: {}", path.generic_string(), std::strerror(errno));
        return false;
    }
}

bool QuickArmorRebalance::IsValidItem(RE::TESBoundObject* i) {
    auto mod = i->GetFile(0);
    if (g_Config.blacklist.contains(mod)) return false;

    if (!i->GetPlayable() || i->IsDeleted() || i->IsIgnored() || !i->GetName() || i->IsDynamicForm()) return false;

    if (auto armor = i->As<RE::TESObjectARMO>()) {
        if (!armor->GetFullName() || armor->GetFullNameLength() <= 0) return false;

        /*
        if (((unsigned int)armor->GetSlotMask() & g_Config.usedSlotsMask) == 0) {
            // logger::debug("Skipping item for no valid slots {}", i->GetFullName());
            return false;
        }
        */
    } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
        if (!weap->GetFullName() || weap->GetFullNameLength() <= 0) return false;

    } else if (auto ammo = i->As<RE::TESAmmo>()) {
        if (!ammo->GetFullName() || ammo->GetFullNameLength() <= 0) return false;
    } else
        return false;

    return true;
}

void ProcessItem(RE::TESBoundObject* i) {
    if (!IsValidItem(i)) return;

    auto mod = i->GetFile(0);
    auto it = g_Data.modData.find(mod);
    ModData* data = nullptr;

    if (it != g_Data.modData.end())
        data = it->second.get();
    else {
        g_Data.sortedMods.push_back(data = (g_Data.modData[mod] = std::make_unique<ModData>(mod)).get());
        logger::trace("Added {}", mod->fileName);
    }

    data->items.insert(i);
}

void CopyRecipe(std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*>& map, RE::TESBoundObject* src,
                RE::TESBoundObject* tar) {
    if (map.find(tar) != map.end()) return;

    auto it = map.find(src);
    if (it != map.end()) map.insert({tar, it->second});
}

void QuickArmorRebalance::ProcessData() {
    auto dataHandler = RE::TESDataHandler::GetSingleton();

    for (auto i : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
        ProcessItem(i);
    }

    for (auto i : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
        ProcessItem(i);
    }

    for (auto i : dataHandler->GetFormArray<RE::TESAmmo>()) {
        ProcessItem(i);
    }

    if (!g_Data.sortedMods.empty()) {
        std::sort(g_Data.sortedMods.begin(), g_Data.sortedMods.end(), [](ModData* const a, ModData* const b) {
            return _stricmp(a->mod->GetFilename().data(), b->mod->GetFilename().data()) < 0;
        });
    }

    auto temperBench = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingArmorTable");
    if (!temperBench) return;

    auto temperWeapBench = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingSharpeningWheel");
    if (!temperWeapBench) return;

    auto smelter = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmelter");
    if (!temperBench) return;

    auto& lsRecipies = dataHandler->GetFormArray<RE::BGSConstructibleObject>();
    for (auto i : lsRecipies) {
        if (!i->createdItem) continue;
        auto pObj = i->createdItem->As<RE::TESBoundObject>();
        if (!pObj) continue;

        if (i->benchKeyword == temperBench || i->benchKeyword == temperWeapBench)
            g_Data.temperRecipe.insert({pObj, i});
        else if (i->benchKeyword == smelter) {
            auto& mats = i->requiredItems;
            if (mats.numContainerObjects == 1) g_Data.smeltRecipe.insert({mats.containerObjects[0]->obj, i});
        } else
            g_Data.craftRecipe.insert({pObj, i});
    }

    if (g_Config.bUseSecondaryRecipes) {
        for (auto& i : g_Config.armorSets) {
            if (!i.strFallbackRecipeSet.empty()) {
                if (auto as = g_Config.FindArmorSet(i.strFallbackRecipeSet.c_str())) {
                    auto fillRecipes = [as](auto item) {
                        bool hasTemper = g_Data.temperRecipe.find(item) != g_Data.temperRecipe.end();
                        bool hasCraft = g_Data.craftRecipe.find(item) != g_Data.craftRecipe.end();

                        if (!hasTemper || !hasCraft) {
                            if (auto copy = as->FindMatching(item)) {
                                if (!hasTemper) CopyRecipe(g_Data.temperRecipe, copy, item);
                                if (!hasCraft) CopyRecipe(g_Data.craftRecipe, copy, item);
                            }
                        }
                    };

                    std::for_each(i.items.begin(), i.items.end(), fillRecipes);
                    std::for_each(i.weaps.begin(), i.weaps.end(), fillRecipes);
                    std::for_each(i.ammo.begin(), i.ammo.end(), fillRecipes);
                } else
                    logger::warn("Fallback recipe set not found : {}", i.strFallbackRecipeSet);
            }

            /*
            auto reportMissingRecipies = [](auto item) {
                if (g_Data.temperRecipe.find(item) == g_Data.temperRecipe.end())
                    logger::info("{}: Missing temper recipe", item->GetName());
                if (g_Data.craftRecipe.find(item) == g_Data.craftRecipe.end())
                    logger::info("{}: Missing craft recipe", item->GetName());
            };

            std::for_each(i.items.begin(), i.items.end(), reportMissingRecipies);
            std::for_each(i.weaps.begin(), i.weaps.end(), reportMissingRecipies);
            std::for_each(i.ammo.begin(), i.ammo.end(), reportMissingRecipies);
            */
        }
    }

    auto& lsAddons = dataHandler->GetFormArray<RE::TESObjectARMA>();
    for (auto addon : lsAddons) {
        if ((addon->GetFormID() & 0xff000000) == 0) {  // Skyrim.esm only

            for (int i = 0; i < RE::SEXES::kTotal; i++) {
                if (!addon->bipedModels[i].model.empty()) {
                    std::string modelPath(addon->bipedModels[i].model);
                    ToLower(modelPath);

                    auto hash = std::hash<std::string>{}(modelPath);
                    g_Data.noModifyModels.insert(hash);

                    if (modelPath.length() > 6) {
                        char* pChar = modelPath.data() + modelPath.length() - 6;  //'_X.nif'
                        if (*pChar++ == '_') {
                            if (*pChar == '0') {
                                *pChar = '1';
                                hash = std::hash<std::string>{}(modelPath);
                                g_Data.noModifyModels.insert(hash);
                            } else if (*pChar == '1') {
                                *pChar = '0';
                                hash = std::hash<std::string>{}(modelPath);
                                g_Data.noModifyModels.insert(hash);
                            }
                        }
                    }
                }
            }
        }
    }
}

void QuickArmorRebalance::LoadChangesFromFiles() {
    /*
    //Iterate from existing mods - probably slower so doing it the other way around
    auto count = data->GetLoadedModCount();
    auto mods = data->GetLoadedMods();

    for (auto i = 0; i < count; i++) {
        logger::info("[{}]: {}", i, mods[i]->fileName);
    }

    count = data->GetLoadedLightModCount();
    mods = data->GetLoadedLightMods();
    for (auto i = 0; i < count; i++) {
        mods[i]->logger::info("[{}]: {}", i, mods[i]->fileName);
    }
    */

    ImportFromDAV();

    logger::info("Loading changes from files");
    LoadChangesFromFolder("shared/", QuickArmorRebalance::g_Config.permShared);
    logger::info("{} items affected from shared changes", g_Data.modifiedItemsShared.size());
    LoadChangesFromFolder("local/", QuickArmorRebalance::g_Config.permLocal);
    logger::info("{} items affected from local changes", g_Data.modifiedItems.size());
}

void QuickArmorRebalance::ForChangesInFolder(const char* sub,
                                             const std::function<void(const RE::TESFile*, std::filesystem::path)> fn) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();

    auto path = std::filesystem::current_path() / PATH_ROOT PATH_CHANGES;
    path /= sub;

    if (!std::filesystem::exists(path)) return;

    if (!std::filesystem::is_directory(path)) {
        logger::error("Is not a directory ({})", path.generic_string());
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        if (_stricmp(entry.path().extension().generic_string().c_str(), ".json")) continue;

        auto modName = entry.path().filename().generic_string();
        modName.resize(modName.size() - 5);  // strip ".json"

        if (auto mod = dataHandler->LookupModByName(modName)) {
            logger::trace("Loading change file {}", entry.path().filename().generic_string());
            fn(mod, entry.path());
        }
    }
}

void QuickArmorRebalance::LoadChangesFromFolder(const char* sub, const Permissions& perm) {
    ForChangesInFolder(sub, [&](auto mod, auto path) {
        if (!LoadFileChanges(mod, path, perm))
            logger::warn("Failed to load change file {}", path.filename().generic_string());
    });
}

bool QuickArmorRebalance::LoadFileChanges(const RE::TESFile* mod, std::filesystem::path path, const Permissions& perm) {
    Document doc;

    if (!ReadJSONFile(path, doc, false)) return false;

    if (doc.HasParseError() || !doc.IsObject()) return false;

    /*
    if (auto fp = std::fopen(path.generic_string().c_str(), "rb")) {
        char readBuffer[1 << 16];
        FileReadStream is(fp, readBuffer, sizeof(readBuffer));
        doc.ParseStream(is);
        std::fclose(fp);

        if (doc.HasParseError()) {
            logger::warn("{}: JSON parse error: {} ({})", path.generic_string(), GetParseError_En(doc.GetParseError()),
                         doc.GetErrorOffset());
            return false;
        }

        if (!doc.IsObject()) {
            logger::warn("{}: Unexpected contents, overwriting previous contents", path.generic_string());
            return false;
        }
    } else {
        logger::warn("{}: Couldn't open file", path.generic_string());
        return false;
    }
    */

    ApplyChanges(mod, doc.GetObj(), perm);

    return true;
}

void QuickArmorRebalance::DeleteAllChanges(RE::TESFile* mod) {
    auto path = std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/";
    path /= mod->fileName;
    path += ".json";

    if (!std::filesystem::exists(path)) return;

    std::filesystem::remove(path);
    g_Data.modifiedFilesDeleted.insert(mod);
}
