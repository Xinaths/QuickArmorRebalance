#include "Config.h"

#include <filesystem>

#include "Data.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#define SETTINGS_FILE "QuickArmorRebalance.toml"

#define USER_BLACKLIST_FILE "User Blacklist.json"

#include "LootLists.h"

using namespace rapidjson;

namespace QuickArmorRebalance {
    Config g_Config;

    RebalanceCurveNode::Tree LoadCurveNode(const Value& node);
    bool LoadArmorSet(BaseArmorSet& s, const Value& node);

    RE::TESObjectARMO* BaseArmorSet::FindMatching(RE::TESObjectARMO* w) const {
        auto slots = (ArmorSlots)w->GetSlotMask();

        auto it = items.begin();

        if (slots & kHeadSlotMask)  // Match with any head slot
            it = std::find_if(items.begin(), items.end(), [=](RE::TESObjectARMO* armor) {
                return (kHeadSlotMask & (ArmorSlots)armor->GetSlotMask());
            });
        else
            it = std::find_if(items.begin(), items.end(),
                              [=](RE::TESObjectARMO* armor) { return (slots & (ArmorSlots)armor->GetSlotMask()); });

        return it != items.end() ? *it : nullptr;
    }

    RE::TESObjectWEAP* BaseArmorSet::FindMatching(RE::TESObjectWEAP* w) const {
        for (auto i : weaps) {
            if (i->GetWeaponType() == w->GetWeaponType()) {
                // 2h maces use the same weapon type as 2h axe, so need to actually compare keywords
                for (unsigned int j = 0; j < i->numKeywords; j++) {
                    if (g_Config.kwSetWeapTypes.contains(i->keywords[j])) {
                        if (w->HasKeyword(i->keywords[j])) return i;
                        break;
                    }
                }
            }
        }
        return nullptr;
    }

    RE::TESAmmo* BaseArmorSet::FindMatching(RE::TESAmmo* w) const {
        for (auto i : ammo) {
            if (i->GetRuntimeData().data.flags.none(RE::AMMO_DATA::Flag::kNonBolt) ==
                w->GetRuntimeData().data.flags.none(RE::AMMO_DATA::Flag::kNonBolt)) {
                return i;
            }
        }
        return nullptr;
    }

}

void LoadPermissions(QuickArmorRebalance::Permissions& p, toml::node_view<toml::node> tbl) {
    p.bDistributeLoot = tbl["loot"].value_or(true);
    p.bModifyKeywords = tbl["modifyKeywords"].value_or(true);
    p.bModifySlots = tbl["modifySlots"].value_or(true);
    p.bModifyArmorRating = tbl["modifyArmor"].value_or(true);
    p.bModifyValue = tbl["modifyValue"].value_or(true);
    p.bModifyWeight = tbl["modifyWeight"].value_or(true);
    p.bModifyWarmth = tbl["modifyWarmth"].value_or(true);
    p.bModifyWeapDamage = tbl["modifyWeapDamage"].value_or(true);
    p.bModifyWeapWeight = tbl["modifyWeapWeight"].value_or(true);
    p.bModifyWeapSpeed = tbl["modifyWeapSpeed"].value_or(true);
    p.bModifyWeapStagger = tbl["modifyWeapStagger"].value_or(true);
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

    bEnableSkyrimWarmthHook = !REL::Module::IsVR();

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
        if (kwSet.empty() || kwSetWeap.empty() || kwSetWeapTypes.empty() || armorSets.empty() || curves.empty() ||
            lootProfiles.empty()) {
            bSuccess = false;
            logger::error("Missing required data, disabling");
        }
    }

    // UI configs
    {
        const char* lootProfile = "Treasure - Universal";

        auto config = toml::parse_file((std::filesystem::current_path() / PATH_ROOT SETTINGS_FILE).generic_string());
        if (config) {
            g_Config.acParams.bMerge = config["merge"].value_or(true);
            g_Config.acParams.bModifyKeywords = config["modifyKeywords"].value_or(true);
            g_Config.acParams.armor.rating.bModify = config["modifyArmor"].value_or(true);
            g_Config.acParams.armor.weight.bModify = config["modifyWeight"].value_or(true);

            g_Config.acParams.weapon.damage.bModify = config["modifyWeapDamage"].value_or(true);
            g_Config.acParams.weapon.weight.bModify = config["modifyWeapWeight"].value_or(true);
            g_Config.acParams.weapon.speed.bModify = config["modifyWeapSpeed"].value_or(true);
            g_Config.acParams.weapon.stagger.bModify = config["modifyWeapStagger"].value_or(true);

            g_Config.acParams.value.bModify = config["modifyValue"].value_or(true);

            auto armorSet = config["armorset"];
            if (armorSet.is_string()) {
                auto str = armorSet.value<std::string>();
                g_Config.acParams.armorSet = FindArmorSet(str->c_str());
                for (auto& i : g_Config.armorSets) {
                    if (!_stricmp(str->c_str(), i.name.c_str())) {
                        g_Config.acParams.armorSet = &i;
                        break;
                    }
                }
            }

            auto curve = config["curve"].value_or("QAR");
            for (auto& i : g_Config.curves) {
                if (!_stricmp(curve, i.first.c_str())) {
                    g_Config.acParams.curve = &i.second;
                    break;
                }
            }

            lootProfile = config["loot"]["profile"].value_or("Treasure - Universal");
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
            g_Config.bEnableRarityNullLoot = config["settings"]["enforcerarity"].value_or(false);
            g_Config.bResetSlotRemap = config["settings"]["resetslotremap"].value_or(true);
            g_Config.bEnableAllItems = config["settings"]["enableallitems"].value_or(false);
            g_Config.bAllowInvalidRemap = config["settings"]["allowinvalidremap"].value_or(false);
            g_Config.bUseSecondaryRecipes = config["settings"]["usesecondaryrecipes"].value_or(true);
            g_Config.fTemperGoldCostRatio =
                std::clamp(config["settings"]["goldcosttemper"].value_or(20.f), 0.0f, 200.0f);
            g_Config.fCraftGoldCostRatio = std::clamp(config["settings"]["goldcostcraft"].value_or(70.f), 0.0f, 200.0f);
            g_Config.bEnableSmeltingRecipes = config["settings"]["enablesmeltingrecipes"].value_or(false);
            g_Config.bEnableSkyrimWarmthHook = config["settings"]["enablewarmthhook"].value_or(!REL::Module::IsVR());
            g_Config.bShowFrostfallCoverage = config["settings"]["showffcoverage"].value_or(false);
            g_Config.bEnableProtectedSlotRemapping = config["settings"]["enableprotectedslotremapping"].value_or(false);
            g_Config.bEnableArmorSlotModelFixHook = config["settings"]["enablearmorslotmodelfixhook"].value_or(true);

            LoadPermissions(g_Config.permLocal, config["localPermissions"]);
            LoadPermissions(g_Config.permShared, config["sharedPermissions"]);

            spdlog::set_level((spdlog::level::level_enum)g_Config.verbosity);
        }

        if (!g_Config.acParams.armorSet) g_Config.acParams.armorSet = &g_Config.armorSets[0];
        if (!g_Config.acParams.curve) g_Config.acParams.curve = &g_Config.curves[0].second;
        if (g_Config.lootProfiles.contains(lootProfile))
            g_Config.acParams.distProfile = g_Config.lootProfiles.find(lootProfile)->c_str();
        else if (!lootProfiles.empty())
            g_Config.acParams.distProfile = lootProfiles.begin()->c_str();
    }

    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler->LookupModByName("Frostfall.esp")) {
        isFrostfallInstalled = true;

        const char* kws[] = {"FrostfallEnableKeywordProtection", "FrostfallIgnore", "FrostfallWarmthPoor", "FrostfallWarmthFair",
                             "FrostfallWarmthGood", "FrostfallWarmthExcellent", "FrostfallWarmthMax",
                             "FrostfallCoveragePoor", "FrostfallCoverageFair", "FrostfallCoverageGood",
                             "FrostfallCoverageExcellent",
                             "FrostfallCoverageMax",
                             
                             /*
                             "FrostfallIsCloakCloth",
                             "FrostfallIsCloakLeather",
                             "FrostfallIsCloakFur",
                             "FrostfallIsWeatherproofAccessory",
                             "FrostfallIsWarmAccessory",
                             "FrostfallExtraHeadCloth",
                             "FrostfallExtraHeadWeatherproof",
                             "FrostfallExtraHeadWarm",
                             "FrostfallExtraCloakCloth",
                             "FrostfallExtraCloakLeather",
                             "FrostfallExtraCloakFur",
                             */
                             nullptr};
        for (auto id = kws; *id; id++) {
            auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(*id);
            if (kw) {
                kwFFSet.insert(kw);
            }
        }

        ffKeywords.enable = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kws[0]);
        ffKeywords.ignore = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kws[1]);
        for (int i = 0; i < 5; i++) {
            ffKeywords.warmth[i] = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kws[2 + i]);
            ffKeywords.coverage[i] = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kws[2 + 5 + i]);
        }
    }

    ValidateLootConfig();

    if (bSuccess)
        strCriticalError.clear();
    else
        strCriticalError = "Failed to load";
    return bSuccess;
}

namespace {
    void ConfigFileWarning(std::filesystem::path path, const char* str) {
        logger::warn("{}: {}", path.filename().generic_string(), str);
    }

    bool LoadKeywords(std::filesystem::path path, const Value& d, const char* field, std::set<RE::BGSKeyword*>& set) {
        const auto& jsonKeywords = d[field];
        if (jsonKeywords.IsArray()) {
            for (const auto& i : jsonKeywords.GetArray()) {
                if (i.IsString()) {
                    auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(i.GetString());
                    if (kw) {
                        set.insert(kw);
                    } else
                        logger::info("Keyword not found: {}", i.GetString());
                }
            }
            return true;
        } else
            ConfigFileWarning(path, std::format("{} expected to be an array", field).c_str());

        return false;
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

    if (d.HasMember("requires")) {
        const auto& jsonRequires = d["requires"];
        if (jsonRequires.IsString()) {
            if (!dataHandler->LookupModByName(jsonRequires.GetString())) {
                logger::debug("{}: Lacking required mod \"{}\", skipping", path.filename().generic_string(),
                              jsonRequires.GetString());
                return true;
            }
        }
    }

    if (d.HasMember("keywords")) {
        LoadKeywords(path, d, "keywords", kwSet);
    }

    if (d.HasMember("keywordsSlotSpecific")) {
        LoadKeywords(path, d, "keywordsSlotSpecific", kwSlotSpecSet);
    }

    if (d.HasMember("keywordsWeapon")) {
        LoadKeywords(path, d, "keywordsWeapon", kwSetWeap);
    }

    if (d.HasMember("keywordsWeaponTypes")) {
        LoadKeywords(path, d, "keywordsWeaponTypes", kwSetWeapTypes);
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
                    if (!(s.items.empty() && s.weaps.empty() && s.ammo.empty()) &&
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

        if (node.HasMember("recipeFallbackSet")) {
            as.strFallbackRecipeSet = node["recipeFallbackSet"].GetString();
        }

        if (node.HasMember("file")) {
            if (auto mod = dataHandler->LookupModByName(node["file"].GetString())) {
                if (node.HasMember("items") && node["items"].IsArray()) {
                    unsigned int covered = 0;
                    for (const auto& i : node["items"].GetArray()) {
                        if (i.IsString()) {
                            auto str = i.GetString();
                            bool bOther;
                            auto item = FindIn(mod, str, &bOther);
                            if (!item) {
                                if (!bOther) logger::error("Item not found for set {}", str);
                                continue;
                            }

                            if (auto armor = item->As<RE::TESObjectARMO>()) {
                                auto slots = (unsigned int)armor->GetSlotMask();
                                slots &= ~kCosmeticSlotMask;  // Most or all hats have an extra hair slot to hide
                                                              // it, but shouldn't be using it

                                if (slots) {
                                    // if ((slots & (slots - 1)) == 0) { //Check there's only one slot used
                                    if (true) {                        // we're only going to use the lowest slot
                                        slots &= slots ^ (slots - 1);  // Turns off all but lowest bit
                                        if (!(covered & slots)) {
                                            covered |= slots;
                                            as.items.insert(armor);
                                        } else {
                                            logger::error("Item covers armor slot that is already covered {:#010x}",
                                                          item->formID);
                                        }
                                    } else
                                        logger::error(
                                            "Item covers multiple slots (not allowed in item sets, except slot 31 "
                                            "(hair)) {:#010x}",
                                            item->formID);
                                } else {
                                    logger::error("Item has no slots {:#010x}", item->formID);
                                }
                            } else if (auto weap = item->As<RE::TESObjectWEAP>()) {
                                if (!as.FindMatching(weap)) {
                                    as.weaps.insert(weap);
                                } else
                                    logger::error("Duplicate weapon type found for item {}", str);
                            } else if (auto ammo = item->As<RE::TESAmmo>()) {
                                if (!as.FindMatching(ammo)) {
                                    as.ammo.insert(ammo);
                                } else
                                    logger::error("Duplicate ammo type found for item {}", str);
                            } else {
                                logger::error("Invalid item type {}", str);
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

    std::vector<const char*> names;
    for (auto i : as.items) names.push_back(i->GetName());
    for (auto i : as.weaps) names.push_back(i->GetName());
    for (auto i : as.ammo) names.push_back(i->GetName());

    std::sort(names.begin(), names.end(), [](const char* a, const char* b) { return _stricmp(a, b) < 0; });
    size_t nLen = 0;
    for (auto i : names) nLen += strlen(i) + 1;

    as.strContents.reserve(nLen);
    for (auto i : names) {
        if (!as.strContents.empty()) as.strContents.append("\n");
        as.strContents.append(i);
    }

    s = std::move(as);
    return bSuccess;
}

toml::table SavePermissions(const QuickArmorRebalance::Permissions& p) {
    return toml::table{
        {"loot", p.bDistributeLoot},
        {"modifyKeywords", p.bModifyKeywords},
        {"modifySlots", p.bModifySlots},
        {"modifyArmor", p.bModifyArmorRating},
        {"modifyWeight", p.bModifyWeight},
        {"modifyWarmth", p.bModifyWarmth},
        {"modifyWeapDamage", p.bModifyWeapDamage},
        {"modifyWeapWeight", p.bModifyWeapWeight},
        {"modifyWeapSpeed", p.bModifyWeapSpeed},
        {"modifyWeapStagger", p.bModifyWeapStagger},
        {"modifyValue", p.bModifyValue},
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
    if (iCurve == g_Config.curves.end()) iCurve = g_Config.curves.begin();

    auto tbl = toml::table{
        {"merge", g_Config.acParams.bMerge},
        {"modifyKeywords", g_Config.acParams.bModifyKeywords},
        {"modifyArmor", g_Config.acParams.armor.rating.bModify},
        {"modifyWeight", g_Config.acParams.armor.weight.bModify},
        {"modifyWeapDamage", g_Config.acParams.weapon.damage.bModify},
        {"modifyWeapWeight", g_Config.acParams.weapon.weight.bModify},
        {"modifyWeapSpeed", g_Config.acParams.weapon.speed.bModify},
        {"modifyWeapStagger", g_Config.acParams.weapon.stagger.bModify},
        {"modifyValue", g_Config.acParams.value.bModify},
        {"armorset", g_Config.acParams.armorSet ? g_Config.acParams.armorSet->name : "error"},
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
                             {"profile", g_Config.acParams.distProfile ? g_Config.acParams.distProfile : "error"}}},
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
             {"enforcerarity", g_Config.bEnableRarityNullLoot},
             {"resetslotremap", g_Config.bResetSlotRemap},
             {"enableallitems", g_Config.bEnableAllItems},
             {"allowinvalidremap", g_Config.bAllowInvalidRemap},
             {"usesecondaryrecipes", g_Config.bUseSecondaryRecipes},
             {"goldcosttemper", g_Config.fTemperGoldCostRatio},
             {"goldcostcraft", g_Config.fCraftGoldCostRatio},
             {"enablesmeltingrecipes", g_Config.bEnableSmeltingRecipes},
             {"enablewarmthhook", g_Config.bEnableSkyrimWarmthHook},
             {"showffcoverage", g_Config.bShowFrostfallCoverage},
             {"enableprotectedslotremapping", g_Config.bEnableProtectedSlotRemapping},
             {"enablearmorslotmodelfixhook", g_Config.bEnableArmorSlotModelFixHook},
         }},
        {"localPermissions", SavePermissions(g_Config.permLocal)},
        {"sharedPermissions", SavePermissions(g_Config.permShared)},

    };

    std::ofstream file(std::filesystem::current_path() / PATH_ROOT SETTINGS_FILE);
    file << tbl;
}

void QuickArmorRebalance::Config::AddUserBlacklist(RE::TESFile* mod) {
    if (blacklist.contains(mod)) return;

    auto path = std::filesystem::current_path() / PATH_ROOT PATH_CONFIGS USER_BLACKLIST_FILE;

    Document d;

    if (auto fp = std::fopen(path.generic_string().c_str(), "rb")) {
        char readBuffer[1 << 16];
        FileReadStream is(fp, readBuffer, sizeof(readBuffer));

        d.ParseStream(is);
        std::fclose(fp);

        if (d.HasParseError()) {
            logger::warn("JSON parse error: {} ({})", GetParseError_En(d.GetParseError()), d.GetErrorOffset());
        }
    }

    if (!d.IsObject()) {
        d.SetObject();
    }
    auto& al = d.GetAllocator();

    if (d.HasMember("blacklist")) {
        auto& jsonblacklist = d["blacklist"];
        if (jsonblacklist.IsArray()) {
            jsonblacklist.GetArray().PushBack(Value(mod->fileName, al), al);
        } else
            d.RemoveMember("blacklist");
    }

    if (!d.HasMember("blacklist")) {
        Value jsonblacklist(kArrayType);
        jsonblacklist.GetArray().PushBack(Value(mod->fileName, al), al);
        d.AddMember("blacklist", jsonblacklist, al);
    }

    if (auto fp = std::fopen(path.generic_string().c_str(), "wb")) {
        char buffer[1 << 16];
        FileWriteStream ws(fp, buffer, sizeof(buffer));
        PrettyWriter<FileWriteStream> writer(ws);
        writer.SetIndent('\t', 1);
        d.Accept(writer);
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

    g_Config.blacklist.insert(mod);

    for (auto i = g_Data.sortedMods.begin(); i != g_Data.sortedMods.end(); i++) {
        if ((*i)->mod == mod) {
            g_Data.sortedMods.erase(i);
            break;
        }
    }
}
