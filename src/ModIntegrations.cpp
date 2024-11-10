#include "ModIntegrations.h"

#include "Config.h"
#include "Data.h"

#define DAV_PATH "Data/SKSE/Plugins/DynamicArmorVariants/"

using namespace rapidjson;
using namespace QuickArmorRebalance;

std::string DAVFormID(const RE::TESFile* file, RE::FormID id) { return std::format("{}|{:X}", file->fileName, id); }

static RE::TESForm* FindDAVFormID(const char* str) {
    if (auto pos = strchr(str, '|')) {
        std::string fileName(str, pos - str);
        if (auto mod = RE::TESDataHandler::GetSingleton()->LookupModByName(fileName)) {
            auto id = GetFullId(mod, (RE::FormID)strtol(pos + 1, nullptr, 16));
            return RE::TESForm::LookupByID(id);
        }
    }
    return nullptr;
}

void QuickArmorRebalance::ExportToDAV(const RE::TESFile* file, const Value& ls, bool bRebuild) {
    if (!g_Config.bEnableDAVExports || (!g_Config.bEnableDAVExportsAlways && !GetModuleHandle(L"DynamicArmorVariants.dll"))) return;

    DynamicVariantSets sets;

    for (auto& i : ls.GetObj()) {
        if (!i.value.IsObject()) continue;

        auto fields = i.value.GetObj();
        if (!fields.HasMember("dynamicVariants")) continue;

        if (!i.name.IsString()) {
            logger::error("Invalid item id in {}", file->fileName);
            continue;
        }

        RE::FormID id = atoi(i.name.GetString());
        auto item = RE::TESForm::LookupByID(GetFullId(file, id));
        if (!item) continue;

        auto armor = item->As<RE::TESObjectARMO>();
        if (!armor) continue;

        auto& jsonDVs = fields["dynamicVariants"];
        if (!jsonDVs.IsObject()) continue;

        for (auto& dv : jsonDVs.GetObj()) {
            if (!dv.value.IsObject()) continue;

            auto it = g_Config.mapDynamicVariants.find(dv.name.GetString());
            if (it != g_Config.mapDynamicVariants.end()) {
                auto dvParams = dv.value.GetObj();
                if (!dvParams.HasMember("base") || !dvParams["base"].IsUint()) continue;
                if (!dvParams.HasMember("stage") || !dvParams["stage"].IsUint()) continue;

                auto base = dvParams["base"].GetUint();
                auto stage = dvParams["stage"].GetInt();

                auto& dvSetMap = sets[&it->second];
                auto& dvSet = dvSetMap[base];

                dvSet.resize(std::max((std::size_t)stage, dvSet.size()));
                dvSet[std::max(0, stage - 1)] = armor;  // stage should never be 0, but just incase
            }
        }
    }

    if (sets.empty()) return;

    std::map<std::string, std::vector<std::pair<std::string, std::string>>> replace;
    std::map<std::string, std::string> displayNames;

    for (auto& dv : sets) {
        for (auto& set : dv.second) {
            auto armorBase = RE::TESForm::LookupByID<RE::TESObjectARMO>(GetFullId(file, (RE::FormID)set.first));
            if (!armorBase) continue;

            auto slots = (ArmorSlots)armorBase->GetSlotMask();
            if (!slots) continue;

            std::erase(set.second, nullptr);

            if (set.second.size() < 1) continue;

            for (int i = 0; i < set.second.size(); i++) {
                std::string group;
                if (set.second.size() == 1) {
                    group = dv.first->DAV.variant;
                    if (dv.first->DAV.perSlot) group += std::format("_Slot{}", GetSlotIndex(slots) + 30);

                    displayNames[group] = dv.first->DAV.display;
                } else {
                    group = std::format("{}_{}of{}", dv.first->DAV.variant, i + 1, set.second.size());
                    if (dv.first->DAV.perSlot) group += std::format("_Slot{}", GetSlotIndex(slots) + 30);

                    displayNames[group] = std::format("{} {}", dv.first->DAV.display, i + 1);
                }

                for (auto addonBase : armorBase->armorAddons) {
                    if (addonBase->GetFile(0) != file) continue;
                    for (auto addonReplace : set.second[i]->armorAddons) {
                        if (addonReplace->GetFile(0) != file) continue;
                        replace[group].emplace_back(DAVFormID(addonBase->GetFile(0), addonBase->GetLocalFormID()),
                                                    DAVFormID(addonReplace->GetFile(0), addonReplace->GetLocalFormID()));
                        break;  // only one allowed each
                    }
                }
            }
        }
    }

    std::filesystem::path path(std::filesystem::current_path() / DAV_PATH);
    std::filesystem::create_directories(path);

    path /= file->fileName;
    path.replace_extension(".json");

    Document doc;
    auto& al = doc.GetAllocator();

    if (std::filesystem::exists(path)) {
        if (!ReadJSONFile(path, doc)) return;

        if (!doc.IsObject()) doc.SetObject();

        auto obj = doc.GetObj();
        if (!obj.HasMember("QARGenerated") || !obj["QARGenerated"].GetBool()) {
            logger::info("Skipping DAV export because file exists and is not generated by QAR: {}", path.generic_string());
            return;
        }
    }

    if (!doc.IsObject()) {
        auto& obj = doc.SetObject();
        obj.AddMember("$schema",
                      "https://raw.githubusercontent.com/Exit-9B/DynamicArmorVariants/main/docs/"
                      "DynamicArmorVariants.schema.json",
                      al);
        obj.AddMember("QARGenerated", true, al);
    }

    const auto& root = doc.GetObj();
    if (!root.HasMember("variants") || !root["variants"].IsArray()) {
        root.RemoveMember("variants");
        root.AddMember("variants", Value(kArrayType), al);
    }

    auto& jsonVariantArray = root["variants"];

    for (auto& i : jsonVariantArray.GetArray()) {
        if (!i.IsObject() || !i.HasMember("name") || !i["name"].IsString()) continue;

        if (bRebuild) {  // Always clear the replacements when rebuilding, but leave other data untouched
            i.RemoveMember("replaceByForm");
        }

        auto it = replace.find(i["name"].GetString());
        if (it == replace.end()) continue;

        if (!i.HasMember("replaceByForm") || !i["replaceByForm"].IsObject()) {
            i.RemoveMember("replaceByForm");
            i.AddMember("replaceByForm", Value(kObjectType), al);
        }

        auto& lsExisting = i["replaceByForm"];
        for (auto& r : it->second) {
            lsExisting.RemoveMember(r.first.c_str());
            lsExisting.AddMember(Value(r.first.c_str(), al), Value(r.second.c_str(), al), al);
        }
        replace.erase(it);
    }

    for (auto& lsReplacements : replace) {
        Value objDV(kObjectType);
        objDV.AddMember("name", Value(lsReplacements.first.c_str(), al), al);
        objDV.AddMember("displayName", Value(displayNames[lsReplacements.first].c_str(), al), al);

        Value objDVList(kObjectType);
        for (auto& r : lsReplacements.second) {
            objDVList.AddMember(Value(r.first.c_str(), al), Value(r.second.c_str(), al), al);
        }
        objDV.AddMember("replaceByForm", objDVList, al);

        jsonVariantArray.PushBack(objDV, al);
    }

    WriteJSONFile(path, doc);
}

void QuickArmorRebalance::ExportAllToDAV() {
    auto ExportFile = [&](auto mod, auto path) {
        Document doc;

        if (!ReadJSONFile(path, doc, false)) return;

        if (doc.HasParseError() || !doc.IsObject()) return;

        ExportToDAV(mod, doc.GetObj(), true);
    };

    ForChangesInFolder("shared/", ExportFile);
    ForChangesInFolder("local/", ExportFile);
}

namespace {
    void ImportDAVFile(std::filesystem::path path) {
        Document doc;
        if (!ReadJSONFile(path, doc)) return;

        if (!doc.IsObject() || !doc.HasMember("variants")) return;

        auto& variants = doc.GetObj()["variants"];
        if (!variants.IsArray()) return;

        for (auto& variant : variants.GetArray()) {
            if (!variant.IsObject() || !variant.HasMember("replaceByForm")) continue;

            auto& replacers = variant["replaceByForm"];
            if (!replacers.IsObject()) continue;
            for (auto& replace : replacers.GetObj()) {
                if (!replace.value.IsString()) continue;

                if (auto form = FindDAVFormID(replace.value.GetString())) {
                    if (auto addon = form->As<RE::TESObjectARMA>()) {
                        g_Data.loot->dynamicVariantsDAV.insert(addon);
                    }
                }
            }
        }
    }
}

void QuickArmorRebalance::ImportFromDAV() {
    if (!g_Data.loot) return;
    if (!GetModuleHandle(L"DynamicArmorVariants.dll")) return;

    logger::info("Importing from DAV files");

    const auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    for (auto& file : dataHandler->files) {
        if (!file) continue;

        auto path = std::filesystem::current_path() / DAV_PATH / file->fileName;
        path.replace_extension(".json");

        if (std::filesystem::exists(path)) {
            ImportDAVFile(path);
        }
    }
}

bool QuickArmorRebalance::ExportToKID(const std::vector<RE::TESBoundObject*>& items, const KeywordChangeMap& map, std::filesystem::path path) {
    auto pathDirs = path;
    pathDirs.remove_filename();
    std::filesystem::create_directories(pathDirs);
    if (auto fp = std::fopen(path.generic_string().c_str(), "wb")) {
        std::unordered_set<RE::TESBoundObject*> validItems(items.begin(), items.end());

        std::fprintf(fp, ";File generated from QAR export\r\n");

        std::string str;
        for (auto& i : map) {
            std::string prefix(std::format("Keyword = {}|", i.first->formEditorID.c_str()));
            for (auto item : i.second.add) {
                if (!validItems.contains(item)) continue;

                str = prefix;
                if (auto armor = item->As<RE::TESObjectARMO>())
                    str += "Armor|";
                else if (auto weapon = item->As<RE::TESObjectWEAP>())
                    str += "Weapon|";
                else if (auto ammo = item->As<RE::TESAmmo>())
                    str += "Ammo|";
                else
                    continue;

                str += std::format("0x{:0X}~{}\r\n", GetFileId(item), item->GetFile(0)->fileName);
                std::fwrite(str.data(), str.size(), 1, fp);
            }
        }

        std::fclose(fp);
        return true;
    } else {
        logger::error("Could not open file to write {}: {}", path.generic_string(), std::strerror(errno));
        return false;
    }
}

template<class T>
std::string BuildFormList(const std::unordered_set<RE::TESBoundObject*>& items, const std::unordered_set<RE::TESBoundObject*>& valid) {
    std::string ret;

    for (auto item : items) {
        if (item->As<T>()) {
            if (valid.contains(item)) {
                if (!ret.empty()) ret += ',';
                ret += std::format("{}|{:X}", item->GetFile(0)->fileName, GetFileId(item));
            }
        }
    }

    return ret;
}

template <class T>
bool ExportSkypatcherFile(const char* type, const KeywordChangeMap& map, std::filesystem::path filename,
                          const std::unordered_set<RE::TESBoundObject*>& validItems) {
    auto pathBase = std::filesystem::current_path() / "Data/SKSE/Plugins/SkyPatcher/";
    auto path = pathBase / type / filename;      

    auto pathFilename = path.filename();
    path.remove_filename();

    std::filesystem::create_directories(path);
    path /= pathFilename;

    std::string str;

    if (auto fp = std::fopen(path.generic_string().c_str(), "wb")) {
        std::fprintf(fp, ";File generated from QAR export\r\n");

        for (auto& i : map) {
            if (!i.second.add.empty()) {
                auto strList = BuildFormList<T>(i.second.add, validItems);
                if (!strList.empty()) {
                    str = std::format("filterBy{}s={}:keywordsToAdd={}\r\n", type, strList, i.first->formEditorID.c_str());
                    std::fwrite(str.data(), 1, str.size(), fp);
                }
            }

            if (!i.second.remove.empty()) {
                auto strList = BuildFormList<T>(i.second.remove, validItems);
                if (!strList.empty()) {
                    str = std::format("filterBy{}s={}:keywordsToRemove={}\r\n", type, strList, i.first->formEditorID.c_str());
                    std::fwrite(str.data(), 1, str.size(), fp);
                }
            }
        }
        std::fclose(fp);
    } else {
        logger::error("Could not open file to write {}: {}", path.generic_string(), std::strerror(errno));
        return false;
    }

    return true;
}

bool QuickArmorRebalance::ExportToSkypatcher(const std::vector<RE::TESBoundObject*>& items, const KeywordChangeMap& map, std::filesystem::path filename) {
    std::unordered_set<RE::TESBoundObject*> validItems(items.begin(), items.end());

    bool bHasArmor = false;
    bool bHasWeapons = false;
    bool bHasAmmo = false;

    for (auto& i : map) {
        for (auto item : i.second.add) {
            if (!validItems.contains(item)) continue;
            if (auto armor = item->As<RE::TESObjectARMO>())
                bHasArmor = true;
            else if (auto weapon = item->As<RE::TESObjectWEAP>())
                bHasWeapons = true;
            else if (auto ammo = item->As<RE::TESAmmo>())
                bHasAmmo = true;
        }

        for (auto item : i.second.remove) {
            if (!validItems.contains(item)) continue;
            if (auto armor = item->As<RE::TESObjectARMO>())
                bHasArmor = true;
            else if (auto weapon = item->As<RE::TESObjectWEAP>())
                bHasWeapons = true;
            else if (auto ammo = item->As<RE::TESAmmo>())
                bHasAmmo = true;
        }
    }

    if (bHasArmor) {
        if (!ExportSkypatcherFile<RE::TESObjectARMO>("Armor", map, filename, validItems)) return false;
    }

    if (bHasWeapons) {
        if (!ExportSkypatcherFile<RE::TESObjectWEAP>("Weapon", map, filename, validItems)) return false;
    }

    if (bHasAmmo) {
        if (!ExportSkypatcherFile<RE::TESAmmo>("Ammo", map, filename, validItems)) return false;
    }

    return true;
}
