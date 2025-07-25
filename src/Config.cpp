#include "Config.h"

#include <filesystem>
// #include <boost/locale.hpp>

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

#include "Enchantments.h"
#include "Localization.h"
#include "LootLists.h"

using namespace rapidjson;
using namespace QuickArmorRebalance;
constexpr auto LZ = QuickArmorRebalance::Localize;

namespace QuickArmorRebalance {
    Config g_Config;

    bool LoadArmorSet(BaseArmorSet& s, const Value& node);
    void LoadCustomKeywords(const Value& jsonCustomKWs);
    bool LoadRegions(const Value& node);

    void ConfigFileWarning(std::filesystem::path path, const char* str) { logger::warn("{}: {}", path.filename().generic_string(), str); }


    RE::TESObjectARMO* BaseArmorSet::FindMatching(RE::TESObjectARMO* w) const {
        auto slots = (ArmorSlots)w->GetSlotMask();

        auto it = items.begin();

        if (slots & kHeadSlotMask)  // Match with any head slot
            it = std::find_if(items.begin(), items.end(), [=](RE::TESObjectARMO* armor) { return (kHeadSlotMask & (ArmorSlots)armor->GetSlotMask()); });
        else
            it = std::find_if(items.begin(), items.end(), [=](RE::TESObjectARMO* armor) { return (slots & (ArmorSlots)armor->GetSlotMask()); });

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
            if (i->GetRuntimeData().data.flags.none(RE::AMMO_DATA::Flag::kNonBolt) == w->GetRuntimeData().data.flags.none(RE::AMMO_DATA::Flag::kNonBolt)) {
                return i;
            }
        }
        return nullptr;
    }

    RebalanceCurve* ArmorChangeParams::curve = nullptr;
    const char* ArmorChangeParams::distProfile = nullptr;

    void ArmorChangeParams::Reset(bool bForce) {
        mapKeywordChanges.clear();
        craft.skipForms.clear();
        temper.skipForms.clear();

        if (bForce || g_Config.bResetSliders) {
            armor.rating.Reset(100.0f);
            armor.weight.Reset(100.0f);
            armor.warmth.Reset(50.0f);
            weapon.damage.Reset(100.0f);
            weapon.speed.Reset(100);
            weapon.weight.Reset(100.0f);
            weapon.stagger.Reset(100.0f);
            ench.power.Reset(100.0f);
            ench.rate.Reset(100.0f);
            ench.pool = nullptr;
            ench.poolChance = 50.0f;
            ench.poolRestrict = false;
            value.Reset(100.0f);
        }
        if (bForce || g_Config.bResetSlotRemap) {
            mapArmorSlots.clear();
        }
    }
    void ArmorChangeParams::Clear() {
        armorSet = nullptr;

        bMerge = true;
        bDistribute = false;
        bDistAsSet = bDistAsPieces = false;
        rarity = -1;
        distProfile = nullptr;
        region = kRegion_KeepPrevious;

        bModifyKeywords = false;
        armor.rating.bModify = false;
        armor.weight.bModify = false;
        armor.warmth.bModify = false;

        weapon.damage.bModify = false;
        weapon.weight.bModify = false;
        weapon.speed.bModify = false;
        weapon.stagger.bModify = false;

        ench.power.bModify = false;
        ench.rate.bModify = false;
        ench.pool = nullptr;

        ench.strip = false;
        ench.stripArmor = ench.stripWeapons = ench.stripStaves = false;

        value.bModify = false;
        temper.bModify = false;
        craft.bModify = false;

        mapArmorSlots.clear();
        mapKeywordChanges.clear();

        temper.modeItems = temper.modePerks = craft.modeItems = craft.modePerks = ArmorChangeParams::eRequirementReplace;
        craft.skipForms.clear();
        temper.skipForms.clear();

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
    p.bModifyCustomKeywords = tbl["modifyCustomKeywords"].value_or(true);
    p.crafting.bModify = tbl["modifyCrafting"].value_or(true);
    p.crafting.bRemove = tbl["removeCrafting"].value_or(true);
    p.crafting.bCreate = tbl["createCrafting"].value_or(true);
    p.crafting.bFree = tbl["freeCrafting"].value_or(true);
    p.temper.bModify = tbl["modifyTemper"].value_or(true);
    p.temper.bRemove = tbl["removeTemper"].value_or(true);
    p.temper.bCreate = tbl["createTemper"].value_or(true);
    p.temper.bFree = tbl["freeTemper"].value_or(true);
    p.bStripEnchArmor = tbl["stripEnchArmor"].value_or(true);
    p.bStripEnchWeapons = tbl["stripEnchWeapons"].value_or(true);
    p.bStripEnchStaves = tbl["stripEnchStaves"].value_or(true);
}

Region* QuickArmorRebalance::Config::GetRegion(const char* name) {
    auto nameHash = std::hash<std::string>{}(MakeLower(name));
    auto& region = g_Config.mapRegions[nameHash];
    if (region.name.empty()) region.name = name;
    return &region;
}


bool QuickArmorRebalance::Config::Load() {
    bEnableConsoleHook = !REL::Module::IsVR();
    bEnableSkyrimWarmthHook = !REL::Module::IsVR();
    g_Config.verbosity = !REL::Module::IsVR() ? spdlog::level::info : spdlog::level::trace;

    {
        /*
        boost::locale::generator gen;
        std::locale loc = gen("");
        std::locale::global(loc);
        */

        Localization::Get()->translations[L"en"] = {};
        wchar_t localeName[85];

        // Get the default user locale name
        if (GetUserDefaultLocaleName(localeName, 85)) {
            // Convert the wide string (wchar_t*) to a narrow string (char*)
            std::wstring ws(localeName);

            // Extract the language code from the locale name
            size_t underscore_pos = ws.find(L'-');
            if (underscore_pos != std::string::npos) ws = ws.substr(0, underscore_pos);
            Localization::Get()->SetTranslation(ws);
        } else
            Localization::Get()->SetTranslation(L"en");
    }

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
        if (kwSet.empty() || kwSetWeap.empty() || kwSetWeapTypes.empty() || armorSets.empty() || curves.empty() || lootProfiles.empty()) {
            bSuccess = false;
            logger::error("Missing required data, disabling");
        }
    }

    auto dataHandler = RE::TESDataHandler::GetSingleton();

    // UI configs
    {
        const char* lootProfile = "Treasure - Universal";

        auto config = toml::parse_file((std::filesystem::current_path() / PATH_ROOT SETTINGS_FILE).generic_string());
        if (config) {
            logger::trace("Loading local settings");
            g_Config.acParams.bMerge = config["merge"].value_or(true);
            g_Config.acParams.bModifyKeywords = config["modifyKeywords"].value_or(true);
            g_Config.acParams.armor.rating.bModify = config["modifyArmor"].value_or(true);
            g_Config.acParams.armor.weight.bModify = config["modifyWeight"].value_or(true);

            g_Config.acParams.weapon.damage.bModify = config["modifyWeapDamage"].value_or(true);
            g_Config.acParams.weapon.weight.bModify = config["modifyWeapWeight"].value_or(true);
            g_Config.acParams.weapon.speed.bModify = config["modifyWeapSpeed"].value_or(true);
            g_Config.acParams.weapon.stagger.bModify = config["modifyWeapStagger"].value_or(true);

            g_Config.acParams.ench.power.bModify = config["modifyEnchPower"].value_or(true);
            g_Config.acParams.ench.rate.bModify = config["modifyEnchRate"].value_or(true);

            g_Config.acParams.value.bModify = config["modifyValue"].value_or(true);

            g_Config.acParams.ench.strip = config["enchStrip"].value_or(false);
            g_Config.acParams.ench.stripArmor = config["enchStripArmor"].value_or(true);
            g_Config.acParams.ench.stripWeapons = config["enchStripWeapons"].value_or(true);
            g_Config.acParams.ench.stripStaves = config["enchStripStaves"].value_or(true);

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

            struct LoadSection {
                static void Crafting(ArmorChangeParams::RecipeOptions& opts, const toml::node_view<toml::node>& tbl) {
                    opts.action = tbl["action"].value_or(ArmorChangeParams::eRecipeModify);
                    opts.bModify = tbl["modify"].value_or(true);
                    opts.bNew = tbl["new"].value_or(false);
                    opts.bFree = tbl["free"].value_or(false);

                    opts.bExpanded = tbl["expand"].value_or(false);
                    opts.modeItems = std::clamp(tbl["opItems"].value_or((int)ArmorChangeParams::eRequirementReplace), 0, ArmorChangeParams::eRequirementModeCount - 1);
                    opts.modePerks = std::clamp(tbl["opPerks"].value_or((int)ArmorChangeParams::eRequirementReplace), 0, ArmorChangeParams::eRequirementModeCount - 1);
                }
            };

            LoadSection::Crafting(g_Config.acParams.temper, config["temper"]);
            LoadSection::Crafting(g_Config.acParams.craft, config["craft"]);

            g_Config.nFontSize = config["settings"]["fontsize"].value_or(13);
            g_Config.verbosity = std::clamp(config["settings"]["verbosity"].value_or((int)g_Config.verbosity), 0, spdlog::level::n_levels - 1);
            g_Config.bCloseConsole = config["settings"]["closeconsole"].value_or(true);
            g_Config.bPauseWhileOpen = config["settings"]["pause"].value_or(true);
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
            g_Config.fTemperGoldCostRatio = std::clamp(config["settings"]["goldcosttemper"].value_or(20.f), 0.0f, 200.0f);
            g_Config.fCraftGoldCostRatio = std::clamp(config["settings"]["goldcostcraft"].value_or(70.f), 0.0f, 200.0f);
            g_Config.bEnableSmeltingRecipes = config["settings"]["enablesmeltingrecipes"].value_or(false);
            g_Config.bEnableConsoleHook = config["settings"]["enableconsolehook"].value_or(bEnableConsoleHook);
            g_Config.bEnableSkyrimWarmthHook = config["settings"]["enablewarmthhook"].value_or(bEnableSkyrimWarmthHook);
            g_Config.bShowFrostfallCoverage = config["settings"]["showffcoverage"].value_or(false);
            g_Config.bEnableProtectedSlotRemapping = config["settings"]["enableprotectedslotremapping"].value_or(false);
            g_Config.bEnableArmorSlotModelFixHook = config["settings"]["enablearmorslotmodelfixhook"].value_or(true);
            g_Config.bPreventDistributionOfDynamicVariants = config["settings"]["nodistdynamicvariants"].value_or(true);
            g_Config.bShowKeywordSlots = config["settings"]["showkeyworditemcats"].value_or(true);
            g_Config.bReorderKeywordsForRelevance = config["settings"]["reorderkeywords"].value_or(true);
            g_Config.bEquipPreviewForKeywords = config["settings"]["equipkeywordpreview"].value_or(true);
            g_Config.bExportUntranslated = config["settings"]["exportuntranslated"].value_or(false);
            g_Config.bEnableEnchantmentDistrib = config["settings"]["distenchants"].value_or(true);
            g_Config.bEnchantRandomCharge = config["settings"]["enchantrandomcharge"].value_or(true);
            g_Config.bAlwaysEnchantStaves = config["settings"]["alwaysenchantstaves"].value_or(true);
            g_Config.fEnchantRates = config["settings"]["enchantrate"].value_or(100.0f);
            g_Config.bShowAllRecipeConditions = config["settings"]["allrecipereqs"].value_or(false);
            g_Config.bEnableRegionalLoot = config["settings"]["regionalloot"].value_or(true);
            g_Config.bEnableCrossRegionLoot = config["settings"]["crossregionloot"].value_or(true);
            g_Config.bEnableMigratedLoot = config["settings"]["migratedloot"].value_or(true);

            if (auto code = config["settings"]["language"].as_string()) {
                if (!code->get().empty()) Localization::Get()->SetTranslation(StringToWString(code->get()));
            }

            if (auto arr = config["settings"]["autodisablewords"].as_array()) {
                g_Config.lsDisableWords.reserve(arr->size());
                arr->for_each([this](auto i) {
                    if (auto str = i.as_string())
                        if (!str->get().empty()) g_Config.lsDisableWords.push_back(str->get());
                });
            }

            if (auto arr = config["settings"]["recipeBlacklistConditions"].as_array()) {
                auto fileDefault = dataHandler->LookupLoadedModByIndex(0);

                arr->for_each([=, this](auto i) {
                    if (auto str = i.as_string())
                        if (!str->get().empty()) {
                            if (auto form = FindIn(fileDefault, str->get().c_str()))
                                g_Config.recipeConditionBlacklist.insert(form);
                            else
                                logger::info("Not found");
                        }
                });
            }

            g_Config.bShortcutEscCloseWindow = config["shortcuts"]["escCloseWindow"].value_or(true);

            g_Config.bEnableDAVExports = config["integrations"]["enableDAVexports"].value_or(true);
            g_Config.bEnableDAVExportsAlways = config["integrations"]["enableDAVexportsalways"].value_or(false);

            g_Config.bEnableBOSDetect = config["integrations"]["enableBOSdetect"].value_or(true);
            g_Config.bEnableBOSFromGeneric = config["integrations"]["enableBOSfromgeneric"].value_or(true);
            g_Config.bEnableBOSFromConditional = config["integrations"]["enableBOSfromconditional"].value_or(true);
            g_Config.bEnableBOSFromReference = config["integrations"]["enableBOSfromreference"].value_or(true);

            LoadPermissions(g_Config.permLocal, config["localPermissions"]);
            LoadPermissions(g_Config.permShared, config["sharedPermissions"]);

            if (auto tbl = config["preferenceVariants"].as_table())
                tbl->for_each([this](const toml::key& key, auto&& val) { mapPrefVariants[std::string(key.str())].pref = (Preference)val.value_or(0); });

            spdlog::set_level((spdlog::level::level_enum)g_Config.verbosity);
        } else
            logger::trace("Local settings file not found");

        // if (!g_Config.acParams.armorSet) g_Config.acParams.armorSet = &g_Config.armorSets[0];
        if (!g_Config.acParams.curve) g_Config.acParams.curve = &g_Config.curves[0].second;
        if (g_Config.lootProfiles.contains(lootProfile))
            g_Config.acParams.distProfile = g_Config.lootProfiles.find(lootProfile)->c_str();
        else if (!lootProfiles.empty())
            g_Config.acParams.distProfile = nullptr; //lootProfiles.begin()->c_str();
    }

    if (dataHandler->LookupModByName("Frostfall.esp")) {
        isFrostfallInstalled = true;

        const char* kws[] = {"FrostfallEnableKeywordProtection", "FrostfallIgnore", "FrostfallWarmthPoor", "FrostfallWarmthFair", "FrostfallWarmthGood", "FrostfallWarmthExcellent",
                             "FrostfallWarmthMax", "FrostfallCoveragePoor", "FrostfallCoverageFair", "FrostfallCoverageGood", "FrostfallCoverageExcellent", "FrostfallCoverageMax",

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

    /*
    std::map<RE::EnchantmentItem*, std::set<RE::EnchantmentItem*>> baseEnchants;

    auto& enchs = dataHandler->GetFormArray<RE::EnchantmentItem>();
    for (auto i : enchs) {
        if (i->data.baseEnchantment == nullptr && i->data.wornRestrictions != nullptr) {
            //logger::info("Ench: {:#10x} {}", i->GetFormID(), i->GetName());
            baseEnchants[i];
        }
    }

    for (auto i : enchs) {
        if (i->data.baseEnchantment) {
            auto base = baseEnchants.find(i->data.baseEnchantment);
            if (base != baseEnchants.end()) base->second.insert(i);
        }
    }

    for (auto& i : baseEnchants) {
        if (!i.second.empty()) {
            logger::info("Base: {}:{:#10x} {}", i.first->GetFile(0)->fileName, i.first->GetFormID(), i.first->GetName());
            for (auto j : i.second) {
                logger::info("--- Ench: {}:{:#10x} {}", j->GetFile(0)->fileName, j->GetFormID(), j->GetName());
            }
        }
    }
    */

    for (const auto& i : mapDynamicVariants) {
        wordsDynamicVariants.insert(i.second.autos.begin(), i.second.autos.end());
        wordsDynamicVariants.insert(i.second.hints.begin(), i.second.hints.end());
    }

    wordsAllVariants.insert(wordsDynamicVariants.begin(), wordsDynamicVariants.end());
    wordsAllVariants.insert(wordsStaticVariants.begin(), wordsStaticVariants.end());
    wordsAllVariants.insert(wordsEitherVariants.begin(), wordsEitherVariants.end());

    ValidateLootConfig();
    RebuildDisabledWords();
    FinalizeEnchantmentConfig();

    for (auto& i : g_Data.distGroups) g_Data.distGroupsSorted.push_back(&i.second);
    std::sort(g_Data.distGroupsSorted.begin(), g_Data.distGroupsSorted.end(), [](auto a, auto b) {
        if (a->level - a->early < b->level - b->early) return true;
        if (a->level < b->level) return true;
        return a < b;
        });


    for (auto& as : armorSets) {
        if (!as.ench.enchPool) continue;

        for (auto i : as.items) {
            mapObjToSet[i] = &as;
        }
        for (auto i : as.weaps) {
            mapObjToSet[i] = &as;
        }
    }

    for (auto& region : g_Config.mapRegions) {
        region.second.rarity.Add(&region.second, eRegion_Same);
    }

    for (auto& group : g_Data.loot->containerGroups) {
        group.second.migration.Add(&group.second, eRegion_Same);
    }

    for (auto& region : mapRegions) {
        if (region.second.IsValid())
            lsRegionsSorted.push_back(&region.second);
    }
    std::sort(lsRegionsSorted.begin(), lsRegionsSorted.end(), [](Region* a, Region* b) { return strcmp(LZ(a->name.c_str()), LZ(b->name.c_str())) < 0; });
    for (auto& strProfile : lootProfiles) {
        auto& regions = lootProfileRegions[strProfile.c_str()];

        for (auto& group : g_Data.loot->distProfiles[strProfile].containerGroups) {
            if (group->regions.empty()) {
                regions.clear();
                break;
            }

            regions.insert(group->regions.begin(), group->regions.end());
        }
    }

    if (bSuccess)
        strCriticalError.clear();
    else
        strCriticalError = "Failed to load";

    logger::trace("Done loading configuration");
    return bSuccess;
}

namespace {

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

    void LoadHashedWordArray(WordSet& set, const char* name, const Value& obj) {
        if (obj.HasMember(name)) {
            const auto& jsonList = obj[name];

            if (jsonList.IsArray()) {
                for (const auto& i : jsonList.GetArray()) {
                    if (i.IsString()) {
                        set.insert(std::hash<std::string>{}(i.GetString()));
                    }
                }
            }
        }
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
                logger::debug("{}: Lacking required mod \"{}\", skipping", path.filename().generic_string(), jsonRequires.GetString());
                return true;
            }
        }
    }

    if (d.HasMember("settings")) {
        const auto& jsonSettings = d["settings"];

        levelEnchDelay = GetJsonInt(jsonSettings, "enchLevelDelay", 0, 10, levelEnchDelay);
        enchChanceBase = GetJsonFloat(jsonSettings, "enchChanceBase", 0.0f, 1.0f, enchChanceBase);
        enchChanceBonus = GetJsonFloat(jsonSettings, "enchChanceBonus", 0.0f, 1.0f, enchChanceBonus);
        enchChanceBonusMax = GetJsonFloat(jsonSettings, "enchChanceBonusMax", 0.0f, 1.0f, enchChanceBonusMax);
        enchWeapChargeMin = GetJsonInt(jsonSettings, "enchWeapChargeMin", 1, 10000, enchWeapChargeMin);
        enchWeapChargeMax = GetJsonInt(jsonSettings, "enchWeapChargeMax", 1, 10000, enchWeapChargeMax);
        flatArmorMod = GetJsonInt(jsonSettings, "flatArmorMod", 1, 10000, flatArmorMod);
        flatValueMod = GetJsonInt(jsonSettings, "flatValueMod", 1, 10000, flatValueMod);
        flatWeightMod = GetJsonInt(jsonSettings, "flatWeightMod", 1, 10000, flatWeightMod);
        flatWeapDamageMod = GetJsonInt(jsonSettings, "flatWeapDamageMod", 1, 10000, flatWeapDamageMod);

        auto ReadRarityParams = [&](const char* name, int* entries) {
            if (jsonSettings.HasMember(name)) {
                auto& node = jsonSettings[name];

                entries[eRegion_Same] = GetJsonInt(node, "same", 0, 100, entries[eRegion_Same]);
                entries[eRegion_Common] = GetJsonInt(node, "common", 0, 100, entries[eRegion_Common]);
                entries[eRegion_Uncommon] = GetJsonInt(node, "uncommon", 0, 100, entries[eRegion_Uncommon]);
                entries[eRegion_Rare] = GetJsonInt(node, "rare", 0, 100, entries[eRegion_Rare]);
                entries[eRegion_Exotic] = GetJsonInt(node, "exotic", 0, 100, entries[eRegion_Exotic]);
            }
        };

        ReadRarityParams("lootRegionWeights", nRegionRarityEntries);
        ReadRarityParams("lootMigrationWeights", nMigrationRarityEntries);
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
                RebalanceCurve curve;
                curve.Load(i.value);
                if (!curve.tree.empty()) curves.push_back({i.name.GetString(), std::move(curve)});
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
                    if (!(s.items.empty() && s.weaps.empty() && s.ammo.empty()) && s.loot) {  // Didn't load correctly, but not a critical failure (probably mod missing)
                        s.name = i.name.GetString();
                        armorSets.push_back(std::move(s));
                    }
                } else
                    logger::warn("{}: Armor set '{}' failed to load", path.filename().generic_string(), i.name.GetString());
            }
        } else
            ConfigFileWarning(path, "sets expected to be an object");
    }

    if (d.HasMember("loot")) {
        LoadLootConfig(d["loot"]);
    }

    if (d.HasMember("regions")) {
        LoadRegions(d["regions"]);
    }

    if (d.HasMember("wordHints")) {
        const auto& jsonHints = d["wordHints"];

        if (jsonHints.IsObject()) {
            LoadHashedWordArray(wordsDynamicVariants, "dynamicVariants", jsonHints);
            LoadHashedWordArray(wordsStaticVariants, "staticVariants", jsonHints);
            LoadHashedWordArray(wordsEitherVariants, "eitherVariants", jsonHints);
            LoadHashedWordArray(wordsPieces, "pieces", jsonHints);
            LoadHashedWordArray(wordsDescriptive, "descriptive", jsonHints);
        } else
            ConfigFileWarning(path, "variantWords expected to be an object");
    }

    if (d.HasMember("dynamicVariants")) {
        const auto& jsonVariants = d["dynamicVariants"];
        if (jsonVariants.IsObject()) {
            for (const auto& i : jsonVariants.GetObj()) {
                auto& dv = mapDynamicVariants[i.name.GetString()];
                if (dv.name.empty()) dv.DAV.display = dv.DAV.variant = dv.name = i.name.GetString();
                LoadHashedWordArray(dv.autos, "AutoWords", i.value);
                LoadHashedWordArray(dv.hints, "HintWords", i.value);

                if (i.value.HasMember("DAV")) {
                    auto& davParams = i.value["DAV"];
                    if (davParams.HasMember("slotSpecific")) dv.DAV.perSlot = davParams["slotSpecific"].GetBool();
                    if (davParams.HasMember("display")) dv.DAV.display = davParams["display"].GetString();
                    if (davParams.HasMember("variant")) dv.DAV.variant = davParams["variant"].GetString();
                }
            }
        } else
            ConfigFileWarning(path, "dynamicVariants expected to be an object");
    }

    if (d.HasMember("preferenceVariants")) {
        const auto& jsonVariants = d["preferenceVariants"];
        if (jsonVariants.IsArray()) {
            for (const auto& i : jsonVariants.GetArray()) {
                if (!i.IsString()) continue;

                auto strWord = i.GetString();

                mapPrefVariants[strWord].hash = std::hash<std::string>{}(MakeLower(strWord));
            }
        } else
            ConfigFileWarning(path, "preferenceVariants expected to be an array");
    }

    if (d.HasMember("customKeywords")) {
        LoadCustomKeywords(d["customKeywords"]);
    }

    if (d.HasMember("translation")) Localization::Get()->LoadTranslation(d["translation"]);

    LoadEnchantmentConfigs(path, d);

    return true;
}

RebalanceCurveNode::Tree LoadCurveNode(RebalanceCurve& curve, const rapidjson::Value& node) {
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

                if (i.HasMember("name") && i["name"].IsString()) curve.slotName[n.slot - 30] = i["name"].GetString();

                if (i.HasMember("weight") && i["weight"].IsInt())
                    n.weight = i["weight"].GetInt();
                else
                    continue;

                if (i.HasMember("children")) n.children = LoadCurveNode(curve, i["children"]);
                v.push_back(std::move(n));
            }
        }
    }

    return v;
}

void RebalanceCurve::Load(const rapidjson::Value& node) {
    const char* strSlotDesc[] = {"Head",       "Hair",   "Body",   "Hands",     "Forearms",         "Amulet",      "Ring",       "Feet",
                                 "Calves",     "Shield", "Tail",   "Long Hair", "Circlet",          "Ears",        "Face",       "Neck",
                                 "Chest",      "Back",   "???",    "Pelvis",    "Decapitated Head", "Decapitate",  "Lower body", "Leg (right)",
                                 "Leg (left)", "Face2",  "Chest2", "Shoulder",  "Arm (left)",       "Arm (right)", "???",        "???"};
    for (int i = 0; i < 32; i++) slotName[i] = strSlotDesc[i];

    tree = LoadCurveNode(*this, node);
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

        if (node.HasMember("enchPool")) {
            auto hash = std::hash<std::string>{}(MakeLower(node["enchPool"].GetString()));
            as.ench.enchPool = &g_Config.mapEnchPools[hash];
        }

        if (node.HasMember("enchPower")) as.ench.enchPower = node["enchPower"].GetFloat();
        if (node.HasMember("enchRate")) as.ench.enchRate = node["enchRate"].GetFloat();

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
                                            logger::error("Item covers armor slot that is already covered {:#010x}", item->formID);
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

void LoadRegionRarityList(Region* region, const Value& node, int rarity, bool bFrom) {
    if (!node.IsArray()) return;

    for (auto& jsonOther : node.GetArray()) {
        if (!jsonOther.IsString()) continue;

        auto other = g_Config.GetRegion(jsonOther.GetString());

        if (other == region) continue;

        if (bFrom) {
            region->rarity.Add(other, rarity);
        } else {
            other->rarity.Add(region, rarity);
        }
    }
}

void LoadRegionList(Region* region, const Value& node, const char* name, bool bFrom) {
    if (!node.HasMember(name)) return;

    auto& rarities = node[name];
    if (!rarities.IsObject()) return;

    static const char* rarityNames[] = {"same", "common", "uncommon", "rare", "exotic", "none"};
    for (int i = 0; i <= eRegion_RarityCount; i++) {
        if (rarities.HasMember(rarityNames[i])) LoadRegionRarityList(region, rarities[rarityNames[i]], i, bFrom);
    }
}

bool QuickArmorRebalance::LoadRegions(const Value& node) {
    if (!node.IsObject()) return false;

    for (auto& jsonRegion : node.GetObj()) {
        if (!jsonRegion.value.IsObject()) continue;

        auto region = g_Config.GetRegion(jsonRegion.name.GetString());
        region->bVerified = true;
        region->name = jsonRegion.name.GetString();
        //region->rarity[eRegion_Same].insert(&region);

        LoadRegionList(region, jsonRegion.value, "from", true);
        LoadRegionList(region, jsonRegion.value, "to", false);

        
    }


    return true; }


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
        {"modifyCustomKeywords", p.bModifyCustomKeywords},
        {"modifyValue", p.bModifyValue},
        {"modifyCrafting", p.crafting.bModify},
        {"createCrafting", p.crafting.bCreate},
        {"freeCrafting", p.crafting.bFree},
        {"modifyTemper", p.temper.bModify},
        {"createTemper", p.temper.bCreate},
        {"freeTemper", p.temper.bFree},
        {"stripEnchArmor", p.bStripEnchArmor},
        {"stripEnchWeapons", p.bStripEnchWeapons},
        {"stripEnchStaves", p.bStripEnchStaves},
    };
}

toml::table CraftingSettings(const ArmorChangeParams::RecipeOptions& opts) {
    return toml::table{{"action", opts.action},    {"modify", opts.bModify},    {"new", opts.bNew},         {"free", opts.bFree},
                       {"expand", opts.bExpanded}, {"opItems", opts.modeItems}, {"opPerks", opts.modePerks}};
}

void QuickArmorRebalance::Config::Save() {
    auto iCurve = g_Config.curves.begin();
    while (iCurve != g_Config.curves.end() && &iCurve->second != g_Config.acParams.curve) iCurve++;
    if (iCurve == g_Config.curves.end()) iCurve = g_Config.curves.begin();

    auto tblPrefVars = toml::table{};
    for (const auto& i : mapPrefVariants) {
        tblPrefVars.insert(i.first, i.second.pref);
    }

    auto tomlDisableWords = toml::array{};
    tomlDisableWords.insert(tomlDisableWords.begin(), lsDisableWords.begin(), lsDisableWords.end());

    auto tomlRecipeConditionBlacklist = toml::array{};
    for (auto i : recipeConditionBlacklist) tomlRecipeConditionBlacklist.push_back(QARFormID(i));

    auto tbl = toml::table{
        {"merge", g_Config.acParams.bMerge},
        {"modifyKeywords", g_Config.acParams.bModifyKeywords},
        {"modifyArmor", g_Config.acParams.armor.rating.bModify},
        {"modifyWeight", g_Config.acParams.armor.weight.bModify},
        {"modifyWeapDamage", g_Config.acParams.weapon.damage.bModify},
        {"modifyWeapWeight", g_Config.acParams.weapon.weight.bModify},
        {"modifyWeapSpeed", g_Config.acParams.weapon.speed.bModify},
        {"modifyWeapStagger", g_Config.acParams.weapon.stagger.bModify},
        {"modifyEnchPower", g_Config.acParams.ench.power.bModify},
        {"modifyEnchRate", g_Config.acParams.ench.rate.bModify},
        {"modifyValue", g_Config.acParams.value.bModify},
        {"enchStrip", g_Config.acParams.ench.strip},
        {"enchStripArmor", g_Config.acParams.ench.stripArmor},
        {"enchStripWeapons", g_Config.acParams.ench.stripWeapons},
        {"enchStripStaves", g_Config.acParams.ench.stripStaves},
        {"armorset", g_Config.acParams.armorSet ? g_Config.acParams.armorSet->name : "error"},
        {"curve", iCurve->first},
        {"temper", CraftingSettings(g_Config.acParams.temper)},
        {"craft", CraftingSettings(g_Config.acParams.craft)},
        {"loot", toml::table{{"enable", g_Config.acParams.bDistribute},
                             {"pieces", g_Config.acParams.bDistAsPieces},
                             {"sets", g_Config.acParams.bDistAsSet},
                             {"matching", g_Config.acParams.bMatchSetPieces},
                             {"profile", g_Config.acParams.distProfile ? g_Config.acParams.distProfile : "error"}}},
        {"settings", toml::table{{"fontsize", g_Config.nFontSize},
                                 {"verbosity", g_Config.verbosity},
                                 {"closeconsole", g_Config.bCloseConsole},
                                 {"pause", g_Config.bPauseWhileOpen},
                                 {"autodelete", g_Config.bAutoDeleteGiven},
                                 {"roundweights", g_Config.bRoundWeight},
                                 {"resetsliders", g_Config.bResetSliders},
                                 {"highlights", g_Config.bHighlights},
                                 {"normalizedrops", g_Config.bNormalizeModDrops},
                                 {"droprate", g_Config.fDropRates},
                                 {"levelgranularity", g_Config.levelGranularity},
                                 {"distenchants", g_Config.bEnableEnchantmentDistrib},
                                 {"enchantrate", g_Config.fEnchantRates},
                                 {"enchantrandomcharge", g_Config.bEnchantRandomCharge},
                                 {"alwaysenchantstaves", g_Config.bAlwaysEnchantStaves},
                                 {"craftingraritymax", g_Config.craftingRarityMax},
                                 {"craftingraritydisable", g_Config.bDisableCraftingRecipesOnRarity},
                                 {"keepcraftingbooks", g_Config.bKeepCraftingBooks},
                                 {"enforcerarity", g_Config.bEnableRarityNullLoot},
                                 {"regionalloot", g_Config.bEnableRegionalLoot},
                                 {"crossregionloot", g_Config.bEnableCrossRegionLoot},
                                 {"migratedloot", g_Config.bEnableMigratedLoot},
                                 {"resetslotremap", g_Config.bResetSlotRemap},
                                 {"enableallitems", g_Config.bEnableAllItems},
                                 {"allowinvalidremap", g_Config.bAllowInvalidRemap},
                                 {"usesecondaryrecipes", g_Config.bUseSecondaryRecipes},
                                 {"goldcosttemper", g_Config.fTemperGoldCostRatio},
                                 {"goldcostcraft", g_Config.fCraftGoldCostRatio},
                                 {"enablesmeltingrecipes", g_Config.bEnableSmeltingRecipes},
                                 {"enableconsolehook", g_Config.bEnableConsoleHook},
                                 {"enablewarmthhook", g_Config.bEnableSkyrimWarmthHook},
                                 {"showffcoverage", g_Config.bShowFrostfallCoverage},
                                 {"enableprotectedslotremapping", g_Config.bEnableProtectedSlotRemapping},
                                 {"enablearmorslotmodelfixhook", g_Config.bEnableArmorSlotModelFixHook},
                                 {"nodistdynamicvariants", g_Config.bPreventDistributionOfDynamicVariants},
                                 {"showkeyworditemcats", g_Config.bShowKeywordSlots},
                                 {"reorderkeywords", g_Config.bReorderKeywordsForRelevance},
                                 {"equipkeywordpreview", g_Config.bEquipPreviewForKeywords},
                                 {"allrecipereqs", g_Config.bShowAllRecipeConditions},
                                 {"autodisablewords", tomlDisableWords},
                                 {"language", WStringToString(Localization::Get()->language)},
                                 {"exportuntranslated", g_Config.bExportUntranslated},
                                 {"recipeBlacklistConditions", tomlRecipeConditionBlacklist}}},
        {"shortcuts", toml::table{{"escCloseWindow", g_Config.bShortcutEscCloseWindow}}},
        {"integrations",
         toml::table{
             {"enableDAVexports", g_Config.bEnableDAVExports},
             {"enableDAVexportsalways", g_Config.bEnableDAVExportsAlways},
             {"enableBOSdetect", g_Config.bEnableBOSDetect},
             {"enableBOSfromgeneric", g_Config.bEnableBOSFromGeneric},
             {"enableBOSfromconditional", g_Config.bEnableBOSFromConditional},
             {"enableBOSfromreference", g_Config.bEnableBOSFromReference},
         }},
        {"localPermissions", SavePermissions(g_Config.permLocal)},
        {"sharedPermissions", SavePermissions(g_Config.permShared)},
        {"preferenceVariants", tblPrefVars},
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

void QuickArmorRebalance::Config::RebuildDisabledWords() {
    wordsAutoDisable.clear();
    for (auto w : lsDisableWords) {
        wordsAutoDisable.insert(std::hash<std::string>{}(w));
    }
}

void QuickArmorRebalance::ImportKeywords(const RE::TESFile* mod, const char* tabName, const std::set<RE::BGSKeyword*>& kws) {
    if (!mod) return;

    auto path = std::filesystem::current_path() / PATH_ROOT PATH_CONFIGS;
    path /= mod->fileName;
    path += " Keywords.json";

    Document doc;
    auto& al = doc.GetAllocator();

    if (!ReadJSONFile(path, doc, false)) {
        logger::warn("Failed to load keywords file {}, contents will be overwritten.", path.filename().generic_string());
    }

    if (!doc.IsObject()) doc.SetObject();
    if (!doc.HasMember("requires")) doc.AddMember("requires", Value(mod->fileName, al), al);

    auto& tabs = EnsureHas(doc.GetObj(), "customKeywords", kObjectType, al);

    auto& insertTab = EnsureHas(tabs.GetObj(), tabName, kObjectType, al);

    // Move any existing keywords to new tab
    for (auto& tab : tabs.GetObj()) {
        if (tab.value == insertTab) continue;
        if (!tab.value.IsObject()) {
            tab.value.SetObject();
            continue;
        }

        std::vector<RE::BGSKeyword*> removeKWs;

        for (auto& kwObj : tab.value.GetObj()) {
            auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kwObj.name.GetString());
            if (kw && kws.contains(kw)) {
                insertTab.RemoveMember(kw->formEditorID.c_str());
                insertTab.AddMember(Value(kw->formEditorID.c_str(), al), tab.value, al);
                removeKWs.push_back(kw);
            }
        }

        for (auto kw : removeKWs) tab.value.RemoveMember(kw->formEditorID.c_str());
    }

    for (auto kw : kws) {
        if (!insertTab.HasMember(kw->formEditorID.c_str())) {
            Value kwObj(kObjectType);
            kwObj.AddMember("name", Value(kw->formEditorID.c_str(), al), al);
            kwObj.AddMember("tooltip", Value("", al), al);
            insertTab.AddMember(Value(kw->formEditorID.c_str(), al), kwObj, al);
        }
    }

    LoadCustomKeywords(tabs);

    WriteJSONFile(path, doc);
}

void QuickArmorRebalance::LoadCustomKeywords(const Value& jsonCustomKWs) {
    if (!jsonCustomKWs.IsObject()) return;

    for (const auto& jsonTab : jsonCustomKWs.GetObj()) {
        if (!jsonTab.value.IsObject()) continue;

        auto& tab = g_Config.mapCustomKWTabs[jsonTab.name.GetString()];
        for (const auto& jsonKW : jsonTab.value.GetObj()) {
            if (!jsonKW.value.IsObject()) continue;

            auto kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(jsonKW.name.GetString());
            if (!kw) continue;

            auto& ckw = g_Config.mapCustomKWs[kw];

            if (std::find(tab.begin(), tab.end(), kw) == tab.end()) tab.push_back(kw);

            ckw.kw = kw;

            if (jsonKW.value.HasMember("name"))
                ckw.name = jsonKW.value["name"].GetString();
            else if (ckw.name.empty())
                ckw.name = kw->formEditorID;

            if (jsonKW.value.HasMember("tooltip")) ckw.tooltip = jsonKW.value["tooltip"].GetString();

            if (jsonKW.value.HasMember("imply")) {
                auto& imply = jsonKW.value["imply"];
                if (imply.IsString()) {
                    if (auto kwImply = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(imply.GetString())) ckw.imply.insert(kwImply);
                } else if (imply.IsArray()) {
                    for (auto& i : imply.GetArray()) {
                        if (i.IsString()) {
                            if (auto kwImply = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(i.GetString())) ckw.imply.insert(kwImply);
                        }
                    }
                }
            }

            if (jsonKW.value.HasMember("exclude")) {
                auto& imply = jsonKW.value["exclude"];
                if (imply.IsString()) {
                    if (auto kwImply = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(imply.GetString())) ckw.exclude.insert(kwImply);
                } else if (imply.IsArray()) {
                    for (auto& i : imply.GetArray()) {
                        if (i.IsString()) {
                            if (auto kwImply = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(i.GetString())) ckw.exclude.insert(kwImply);
                        }
                    }
                }
            }

            if (jsonKW.value.HasMember("slots")) {
                auto& slots = jsonKW.value["slots"];
                if (slots.IsUint()) {
                    auto slot = slots.GetUint();
                    if (slot >= 30 && slot < 62) ckw.commonSlots |= (1ull << (slot - 30));
                } else if (slots.IsArray()) {
                    for (auto& i : slots.GetArray()) {
                        if (i.IsUint()) {
                            auto slot = i.GetUint();
                            if (slot >= 30 && slot < 62) ckw.commonSlots |= (1ull << (slot - 30));
                        }
                    }
                }
            }
        }

        // This sort will run too many times but probably a non-issue
        std::sort(tab.begin(), tab.end(),
                  [](RE::BGSKeyword* const a, RE::BGSKeyword* const b) { return _stricmp(g_Config.mapCustomKWs[a].name.c_str(), g_Config.mapCustomKWs[b].name.c_str()) < 0; });
    }
}
