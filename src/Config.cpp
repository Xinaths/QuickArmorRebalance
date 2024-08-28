#include "Config.h"

#include <filesystem>

#include "Data.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/stringbuffer.h"

#define SETTINGS_FILE "QuickArmorReplacer.toml"

#include "LootLists.h"

using namespace rapidjson;

namespace QuickArmorRebalance {
    Config g_Config;

    RebalanceCurveNode::Tree LoadCurveNode(const Value& node);
    bool LoadArmorSet(BaseArmorSet& s, const Value& node);
}

void LoadPermissions(QuickArmorRebalance::Permissions& p, toml::node_view<toml::node> tbl) {
    p.bDistributeLoot = tbl["loot"].value_or(true);
    p.bModifyKeywords = tbl["modifyKeywords"].value_or(true);
    p.bModifyArmorRating = tbl["modifyArmor"].value_or(true);
    p.bModifyValue = tbl["modifyValue"].value_or(true);
    p.bModifyWeight = tbl["modifyWeight"].value_or(true);
    p.crafting.bModify = tbl["modifyCrafting"].value_or(true);
    p.crafting.bCreate = tbl["createCrafting"].value_or(true);
    p.crafting.bFree = tbl["freeCrafting"].value_or(true);
    p.temper.bModify = tbl["modifyTemper"].value_or(true);
    p.temper.bCreate = tbl["createTemper"].value_or(true);
    p.temper.bFree = tbl["freeTemper"].value_or(true);
}

bool QuickArmorRebalance::Config::Load() {
    bool bSuccess = false;
    auto pathConfig = std::filesystem::current_path() / PATH_ROOT PATH_CONFIGS;

    if (!std::filesystem::exists(pathConfig) || !std::filesystem::is_directory(pathConfig)) {
        logger::error("Config file directory missing ({})", pathConfig.generic_string());
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(pathConfig)) {
        if (!entry.is_regular_file()) continue;
        if (_stricmp(entry.path().extension().generic_string().c_str(), ".json")) continue;

        logger::debug("Loading config file {}", entry.path().filename().generic_string());
        if (LoadFile(entry.path())) {
            bSuccess = true;
        } else {
            logger::warn("Failed to load config file {}", entry.path().filename().generic_string());
        }
    }

    if (!bSuccess) {
        logger::error("Failed to read any config files");
    } else {
        if (kwSet.empty() || armorSets.empty() || curves.empty() || lootProfiles.empty()) {
            bSuccess = false;
            logger::error("Missing required data, disabling");
        }
    }

    // UI configs
    {
        auto config = toml::parse_file((std::filesystem::current_path() / PATH_ROOT SETTINGS_FILE).generic_string());
        if (config) {
            g_Config.acParams.bModifyKeywords = config["modifyKeywords"].value_or(true);
            g_Config.acParams.bModifyArmor = config["modifyArmor"].value_or(true);
            g_Config.acParams.bModifyValue = config["modifyValue"].value_or(true);
            g_Config.acParams.bModifyWeight = config["modifyWeight"].value_or(true);

            auto armorSet = config["armorset"];
            if (armorSet.is_string()) {
                auto str = armorSet.value<std::string>();
                for (auto& i : g_Config.armorSets) {
                    if (!_stricmp(str->c_str(), i.name.c_str())) {
                        g_Config.acParams.armorSet = &i;
                        break;
                    }
                }
            }

            if (!g_Config.acParams.armorSet) g_Config.acParams.armorSet = &g_Config.armorSets[0];

            auto curve = config["curve"].value_or("QAR");
            for (auto& i : g_Config.curves) {
                if (!_stricmp(curve, i.first.c_str())) {
                    g_Config.acParams.curve = &i.second;
                    break;
                }
            }
            if (!g_Config.acParams.curve) g_Config.acParams.curve = &g_Config.curves[0].second;

            auto loot = config["loot"]["profile"].value_or("Universal");
            if (g_Config.lootProfiles.contains(loot))
                g_Config.acParams.distProfile = g_Config.lootProfiles.find(loot)->c_str();
            else if (!lootProfiles.empty())
                g_Config.acParams.distProfile = lootProfiles.begin()->c_str();
            g_Config.acParams.bDistribute = config["loot"]["enable"].value_or(false);
            g_Config.acParams.bDistAsPieces = config["loot"]["pieces"].value_or(true);
            g_Config.acParams.bDistAsSet = config["loot"]["sets"].value_or(true);
            g_Config.acParams.bMatchSetPieces = config["loot"]["matching"].value_or(true);

            g_Config.acParams.temper.bModify = config["temper"]["modify"].value_or(true);
            g_Config.acParams.temper.bNew = config["temper"]["new"].value_or(false);
            g_Config.acParams.temper.bFree = config["temper"]["free"].value_or(false);

            g_Config.acParams.craft.bModify = config["craft"]["modify"].value_or(true);
            g_Config.acParams.craft.bNew = config["craft"]["new"].value_or(false);
            g_Config.acParams.craft.bFree = config["craft"]["free"].value_or(false);

            g_Config.verbosity = std::clamp(config["settings"]["verbosity"].value_or((int)spdlog::level::info), 0,
                                            spdlog::level::n_levels - 1);
            g_Config.bCloseConsole = config["settings"]["closeconsole"].value_or(true);
            g_Config.bAutoDeleteGiven = config["settings"]["autodelete"].value_or(false);
            g_Config.bRoundWeight = config["settings"]["roundweights"].value_or(false);
            g_Config.bResetSliders = config["settings"]["resetsliders"].value_or(true);
            g_Config.bHighlights = config["settings"]["highlights"].value_or(true);
            g_Config.bNormalizeModDrops = config["settings"]["normalizedrops"].value_or(true);
            g_Config.fDropRates = config["settings"]["droprate"].value_or(100.0f);
            g_Config.levelGranularity = std::clamp(config["settings"]["levelgranularity"].value_or(3), 1, 5);
            g_Config.craftingRarityMax = std::clamp(config["settings"]["craftingraritymax"].value_or(2), 0, 2);
            g_Config.bDisableCraftingRecipesOnRarity = config["settings"]["craftingraritydisable"].value_or(false);
            g_Config.bKeepCraftingBooks = config["settings"]["keepcraftingbooks"].value_or(false);

            LoadPermissions(g_Config.permLocal, config["localPermissions"]);
            LoadPermissions(g_Config.permShared, config["sharedPermissions"]);

            spdlog::set_level((spdlog::level::level_enum)g_Config.verbosity);
        }
    }

    ValidateLootConfig();

    return bValid = bSuccess;
}

namespace {
    void ConfigFileWarning(std::filesystem::path path, const char* str) {
        logger::warn("{}: {}", path.filename().generic_string(), str);
    }
}

bool QuickArmorRebalance::Config::LoadFile(std::filesystem::path path) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();

    auto fp = std::fopen(path.generic_string().c_str(), "rb");

    if (!fp) {
        logger::warn("Could not open config file {}", path.filename().generic_string());
        return false;
    }

    char readBuffer[1 << 16];
    FileReadStream is(fp, readBuffer, sizeof(readBuffer));

    Document d;
    d.ParseStream(is);
    std::fclose(fp);

    if (d.HasParseError()) {
        logger::warn("JSON parse error: {} ({})", GetParseError_En(d.GetParseError()), d.GetErrorOffset());
        return false;
    }

    if (!d.IsObject()) {
        logger::warn("root is not object");
        return false;
    }

    if (d.HasMember("blacklist")) {
        const auto& jsonblacklist = d["blacklist"];
        if (jsonblacklist.IsArray()) {
            for (const auto& i : jsonblacklist.GetArray()) {
                if (i.IsString()) {
                    auto mod = dataHandler->LookupModByName(i.GetString());
                    if (mod) {
                        logger::debug("Blacklisting {}", mod->fileName);
                        this->blacklist.insert(mod);
                    }
                }
            }
        } else
            ConfigFileWarning(path, "blacklist expected to be an array");
    }

    if (d.HasMember("keywords")) {
        const auto& jsonKeywords = d["keywords"];
        if (jsonKeywords.IsArray()) {
            for (const auto& i : jsonKeywords.GetArray()) {
                if (i.IsString()) {
                    auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(i.GetString());
                    if (kw) {
                        if (!kwSet.contains(kw)) {
                            kwSet.insert(kw);
                        }
                    } else
                        logger::info("Keyword not found: {}", i.GetString());
                }
            }
        } else
            ConfigFileWarning(path, "keywords expected to be an array");
    }

    if (d.HasMember("keywordsSlotSpecific")) {
        const auto& jsonKeywords = d["keywordsSlotSpecific"];
        if (jsonKeywords.IsArray()) {
            for (const auto& i : jsonKeywords.GetArray()) {
                if (i.IsString()) {
                    auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(i.GetString());
                    if (kw) {
                        if (!kwSlotSpecSet.contains(kw)) {
                            kwSlotSpecSet.insert(kw);
                        }
                    } else
                        logger::info("Keyword not found: {}", i.GetString());
                }
            }
        } else
            ConfigFileWarning(path, "keywords expected to be an array");
    }

    if (d.HasMember("curves")) {
        const auto& jsonCurves = d["curves"];

        if (jsonCurves.IsObject()) {
            for (const auto& i : jsonCurves.GetObj()) {
                auto curve{LoadCurveNode(i.value)};
                if (!curve.empty()) curves.push_back({i.name.GetString(), std::move(curve)});
            }
        } else
            ConfigFileWarning(path, "curves expected to be an object");
    }

    if (d.HasMember("sets")) {
        const auto& jsonSets = d["sets"];
        if (jsonSets.IsObject()) {
            for (const auto& i : jsonSets.GetObj()) {
                BaseArmorSet s;
                if (LoadArmorSet(s, i.value)) {
                    if (!s.items.empty() &&
                        s.loot) {  // Didn't load correctly, but not a critical failure (probably mod missing)
                        s.name = i.name.GetString();
                        armorSets.push_back(std::move(s));
                    }
                } else
                    logger::warn("{}: Armor set '{}' failed to load", path.filename().generic_string(),
                                 i.name.GetString());
            }
        } else
            ConfigFileWarning(path, "sets expected to be an object");
    }

    if (d.HasMember("loot")) {
        LoadLootConfig(d["loot"]);
    }

    return true;
}

QuickArmorRebalance::RebalanceCurveNode::Tree QuickArmorRebalance::LoadCurveNode(const Value& node) {
    RebalanceCurveNode::Tree v;

    if (node.IsArray()) {
        v.reserve(node.GetArray().Size());

        for (const auto& i : node.GetArray()) {
            if (i.IsObject()) {
                RebalanceCurveNode n;
                if (i.HasMember("slot") && i["slot"].IsInt()) {
                    n.slot = i["slot"].GetInt();
                    if (n.slot < 30 || n.slot >= 62) continue;

                    g_Config.usedSlotsMask |= (1 << (n.slot - 30));
                } else
                    continue;

                if (i.HasMember("weight") && i["weight"].IsInt())
                    n.weight = i["weight"].GetInt();
                else
                    continue;

                if (i.HasMember("children")) n.children = LoadCurveNode(i["children"]);
                v.push_back(std::move(n));
            }
        }
    }

    return v;
}

bool QuickArmorRebalance::LoadArmorSet(BaseArmorSet& s, const Value& node) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();

    BaseArmorSet as;
    bool bSuccess = true;

    if (node.IsObject()) {
        if (node.HasMember("loot")) {
            as.loot = &g_Data.distGroups[node["loot"].GetString()];
        } else {
            logger::error("Item set missing 'loot' data");
            return false;
        }

        if (node.HasMember("file")) {
            if (auto mod = dataHandler->LookupModByName(node["file"].GetString())) {
                if (node.HasMember("items") && node["items"].IsArray()) {
                    unsigned int covered = 0;
                    for (const auto& i : node["items"].GetArray()) {
                        if (i.IsString()) {
                            auto str = i.GetString();
                            if (auto item = FindIn<RE::TESObjectARMO>(mod, str)) {
                                auto slots = (unsigned int)item->GetSlotMask();
                                slots &= ~kCosmeticSlotMask;  // Most or all hats have an extra hair slot to hide
                                                              // it, but shouldn't be using it

                                if (slots) {
                                    // if ((slots & (slots - 1)) == 0) { //Check there's only one slot used
                                    if (true) {                        // we're only going to use the lowest slot
                                        slots &= slots ^ (slots - 1);  // Turns off all but lowest bit
                                        if (!(covered & slots)) {
                                            covered |= slots;
                                            as.items.insert(item);
                                        } else {
                                            logger::error("Item covers armor slot that is already covered {:#010x}",
                                                          item->formID);
                                            bSuccess = false;
                                        }
                                    } else
                                        logger::error(
                                            "Item covers multiple slots (not allowed in item sets, except slot 31 "
                                            "(hair)) {:#010x}",
                                            item->formID);
                                } else {
                                    logger::error("Item has no slots {:#010x}", item->formID);
                                    bSuccess = false;
                                }
                            } else {
                                logger::error("Item not found for set {}", str);
                                bSuccess = false;
                            }
                        }
                    }
                }
            } else {
                logger::debug("Armor set file missing ({}), skipping", node["file"].GetString());
                return true;
            }
        } else {
            logger::error("Armor set 'file' missing");
            return false;
        }
    }

    s = std::move(as);
    return bSuccess;
}

toml::table SavePermissions(const QuickArmorRebalance::Permissions& p) {
    return toml::table{
        {"loot", p.bDistributeLoot},
        {"modifyKeywords", p.bModifyKeywords},
        {"modifyArmor", p.bModifyArmorRating},
        {"modifyValue", p.bModifyValue},
        {"modifyWeight", p.bModifyWeight},
        {"modifyCrafting", p.crafting.bModify},
        {"createCrafting", p.crafting.bCreate},
        {"freeCrafting", p.crafting.bFree},
        {"modifyTemper", p.temper.bModify},
        {"createTemper", p.temper.bCreate},
        {"freeTemper", p.temper.bFree},
    };
}

void QuickArmorRebalance::Config::Save() {
    auto iCurve = g_Config.curves.begin();
    while (iCurve != g_Config.curves.end() && &iCurve->second != g_Config.acParams.curve) iCurve++;

    auto tbl = toml::table{
        {"modifyKeywords", g_Config.acParams.bModifyKeywords},
        {"modifyArmor", g_Config.acParams.bModifyArmor},
        {"modifyValue", g_Config.acParams.bModifyValue},
        {"modifyWeight", g_Config.acParams.bModifyWeight},
        {"armorset", g_Config.acParams.armorSet->name},
        {"curve", iCurve->first},
        {"temper", toml::table{{"modify", g_Config.acParams.temper.bModify},
                               {"new", g_Config.acParams.temper.bNew},
                               {"free", g_Config.acParams.temper.bFree}}},
        {"craft", toml::table{{"modify", g_Config.acParams.craft.bModify},
                              {"new", g_Config.acParams.craft.bNew},
                              {"free", g_Config.acParams.craft.bFree}}},
        {"loot", toml::table{{"enable", g_Config.acParams.bDistribute},
                             {"pieces", g_Config.acParams.bDistAsPieces},
                             {"sets", g_Config.acParams.bDistAsSet},
                             {"matching", g_Config.acParams.bMatchSetPieces},
                             {"profile", g_Config.acParams.distProfile}}},
        {"settings",
         toml::table{
             {"verbosity", g_Config.verbosity},
             {"closeconsole", g_Config.bCloseConsole},
             {"autodelete", g_Config.bAutoDeleteGiven},
             {"roundweights", g_Config.bRoundWeight},
             {"resetsliders", g_Config.bResetSliders},
             {"highlights", g_Config.bHighlights},
             {"normalizedrops", g_Config.bNormalizeModDrops},
             {"droprate", g_Config.fDropRates},
             {"levelgranularity", g_Config.levelGranularity},
             {"craftingraritymax", g_Config.craftingRarityMax},
             {"craftingraritydisable", g_Config.bDisableCraftingRecipesOnRarity},
             {"keepcraftingbooks", g_Config.bKeepCraftingBooks},
         }},
        {"localPermissions", SavePermissions(g_Config.permLocal)},
        {"sharedPermissions", SavePermissions(g_Config.permShared)},

    };

    std::ofstream file(std::filesystem::current_path() / PATH_ROOT SETTINGS_FILE);
    file << tbl;
}
