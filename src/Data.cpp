#include "Data.h"

#include "ArmorChanger.h"
#include "Config.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/stringbuffer.h"

using namespace rapidjson;

namespace QuickArmorRebalance {
    void LoadChangesFromFolder(const char* sub, const Permissions& perm);
    bool LoadFileChanges(const RE::TESFile* mod, std::filesystem::path path, const Permissions& perm);
}

using namespace QuickArmorRebalance;

ProcessedData QuickArmorRebalance::g_Data;

bool QuickArmorRebalance::IsValidArmor(RE::TESObjectARMO* i) { 
     auto mod = i->GetFile(0);
    if (g_Config.blacklist.contains(mod)) return false;

    if (!i->GetPlayable() || i->IsDeleted() || i->IsIgnored() || !i->GetFullName() || i->IsDynamicForm() || i->GetFullNameLength() <= 0)
        return false;

    if (((unsigned int)i->GetSlotMask() & g_Config.usedSlotsMask) == 0) {
        //logger::debug("Skipping item for no valid slots {}", i->GetFullName());
        return false;
    }

    return true; 
}

void QuickArmorRebalance::ProcessData() {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    auto& lsArmor = dataHandler->GetFormArray<RE::TESObjectARMO>();

    logger::trace("Searching armor list for eligable mods");
    for (auto i : lsArmor) {
        if (!IsValidArmor(i)) continue;

        auto mod = i->GetFile(0);
        auto it = g_Data.modData.find(mod);
        ModData* data = nullptr;

        if (it != g_Data.modData.end())
            data = it->second.get();
        else {
            g_Data.sortedMods.push_back(data = (g_Data.modData[mod] = std::make_unique<ModData>(mod)).get());
            logger::trace("Added {}", mod->fileName);
        }

        data->armors.insert(i);
    }

    if (!g_Data.sortedMods.empty()) {
        std::sort(g_Data.sortedMods.begin(), g_Data.sortedMods.end(), [](ModData* const a, ModData* const b) {
            return _stricmp(a->mod->GetFilename().data(), b->mod->GetFilename().data()) < 0;
        });
    }

    auto temperBench = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingArmorTable");
    if (!temperBench) return;

    auto& lsRecipies = dataHandler->GetFormArray<RE::BGSConstructibleObject>();
    for (auto i : lsRecipies) {
        if (!i->createdItem) continue;
        auto pArmor = i->createdItem->As<RE::TESObjectARMO>();
        if (!pArmor) continue;

        if (i->benchKeyword == temperBench)
            g_Data.temperRecipe.insert({pArmor, i});
        else
            g_Data.craftRecipe.insert({pArmor, i});
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

    logger::info("Loading changes from files");
    LoadChangesFromFolder("shared/", QuickArmorRebalance::g_Config.permShared);
    logger::info("{} items affected from shared changes", g_Data.modifiedItemsShared.size());
    LoadChangesFromFolder("local/", QuickArmorRebalance::g_Config.permLocal);
    logger::info("{} items affected from local changes", g_Data.modifiedItems.size());
}

void QuickArmorRebalance::LoadChangesFromFolder(const char* sub, const Permissions& perm) {
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
            if (!LoadFileChanges(mod, entry.path(), perm))
                logger::warn("Failed to load change file {}", entry.path().filename().generic_string());
        }
    }
}

bool QuickArmorRebalance::LoadFileChanges(const RE::TESFile* mod, std::filesystem::path path, const Permissions& perm) {
    Document doc;

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

