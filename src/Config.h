#pragma once

#include "ArmorSetBuilder.h"
#include "Data.h"

#define PATH_ROOT "Data/SKSE/Plugins/" PLUGIN_NAME "/"
#define PATH_CONFIGS "config/"
#define PATH_CHANGES "changes/"
#define PATH_CUSTOMKEYWORDS "custom keywords/"

namespace QuickArmorRebalance {

    enum Preference { Pref_Ignore, Pref_With, Pref_Without };

    struct LootDistGroup;
    struct EnchantmentPool;

    const unsigned int kCosmeticSlotMask = 0;
    //(unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHead | (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHair;

    const ArmorSlots kHeadSlotMask =
        (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHair | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kCirclet;

    const ArmorSlots kProtectedSlotMask = (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kBody | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHands |
                                          (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kFeet | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead;

    struct RebalanceCurveNode {
        using Tree = std::vector<RebalanceCurveNode>;

        ArmorSlots GetSlots() const { return 1 << (slot - 30); }

        ArmorSlot slot = 0;
        int weight = 0;
        Tree children;
    };

    struct RebalanceCurve {
        RebalanceCurveNode::Tree tree;
        std::string slotName[32];

        void Load(const rapidjson::Value& node);
    };

    struct BaseArmorSet {
        std::string name;
        std::string strContents;
        std::string strFallbackRecipeSet;
        LootDistGroup* loot = nullptr;
        std::set<RE::TESObjectARMO*> items;
        std::set<RE::TESObjectWEAP*> weaps;
        std::set<RE::TESAmmo*> ammo;

        EnchantParams ench;

        RE::TESObjectARMO* FindMatching(RE::TESObjectARMO* w) const;
        RE::TESObjectWEAP* FindMatching(RE::TESObjectWEAP* w) const;
        RE::TESAmmo* FindMatching(RE::TESAmmo* w) const;
    };

    struct ArmorChangeData {
        std::vector<RE::TESBoundObject*> filteredItems;
        std::vector<RE::TESBoundObject*> items;

        bool isWornArmor = false;
        mutable bool bMixedSetDone = false;

        AnalyzeResults analyzeResults;
        DynamicVariantSets dvSets;
    };

    struct ArmorChangeParams {
        ArmorChangeParams(ArmorChangeData& data) : data(&data) {}
        ArmorChangeData* data = nullptr;

        BaseArmorSet* armorSet = nullptr;
        static RebalanceCurve* curve;
        static const char* distProfile;

        struct SliderPair {
            void Reset(float def) { fScale = bFlat ? 0.0f : def; }
            bool IsDefault(float def = 100.0f) const { return fScale == (bFlat ? 0.0f : def); }

            float fScale = 100.0f;
            bool bModify = true;
            bool bFlat = false;
        };

        bool bModifyKeywords = true;

        struct {
            SliderPair rating;
            SliderPair weight;
            SliderPair warmth{50.0f};
            float coverage = 50.0f;
        } armor;

        struct {
            SliderPair damage;
            SliderPair weight;
            SliderPair speed;
            SliderPair stagger;
        } weapon;

        struct {
            SliderPair power;
            SliderPair rate;
            EnchantmentPool* pool = nullptr;
            float poolChance = 50.0f;
            bool poolRestrict = false;

            bool strip = false;
            bool stripWeapons = true;
            bool stripArmor = true;
            bool stripStaves = true;
        } ench;

        SliderPair value;

        enum RecipeAction {
            eRecipeModify,
            eRecipeRemove,
            RecipeActionCount
        };

        enum RecipeRequirementMode {
            eRequirementReplace,
            eRequirementKeep,
            eRequirementRemove,
            eRequirementModeCount
        };

        struct RecipeOptions {
            int action = eRecipeModify;
            bool bModify = true;
            bool bNew = false;
            bool bFree = false;

            bool bExpanded = false;

            int modePerks = eRequirementReplace;
            int modeItems = eRequirementReplace;
            std::set<RE::TESForm*> skipForms;
        };

        RecipeOptions temper;
        RecipeOptions craft;

        int rarity = 0;
        bool bDistribute = false;
        bool bDistAsSet = true;
        bool bDistAsPieces = true;
        bool bMatchSetPieces = false;
        bool bMerge = true;

        mutable ArmorSlots remapMask = 0;
        std::map<int, int> mapArmorSlots;

        KeywordChangeMap mapKeywordChanges;

        void Reset(bool bForce = false);  // Resets sliders etc. to defaults
        void Clear();  // Resets to a modificationless state
    };

    struct Permissions {
        struct RecipePermissions {
            bool bModify = true;
            bool bRemove = true;
            bool bCreate = true;
            bool bFree = true;
        };

        bool bDistributeLoot = true;
        bool bModifyKeywords = true;
        bool bModifySlots = true;
        bool bModifyArmorRating = true;
        bool bModifyValue = true;
        bool bModifyWeight = true;
        bool bModifyWarmth = true;
        bool bModifyWeapDamage = true;
        bool bModifyWeapWeight = true;
        bool bModifyWeapSpeed = true;
        bool bModifyWeapStagger = true;
        bool bModifyCustomKeywords = true;
        bool bStripEnchArmor = true;
        bool bStripEnchWeapons = true;
        bool bStripEnchStaves = true;

        RecipePermissions crafting;
        RecipePermissions temper;
    };

    struct CustomKeyword {
        RE::BGSKeyword* kw = nullptr;
        std::string name;
        std::string tooltip;

        std::set<RE::BGSKeyword*> imply;
        std::set<RE::BGSKeyword*> exclude;

        uint64_t commonSlots = 0;
    };

    struct Config {
        bool Load();
        bool LoadFile(std::filesystem::path path);

        void Save();

        void AddUserBlacklist(RE::TESFile* mod);

        BaseArmorSet* FindArmorSet(const char* name) {
            auto it = std::find_if(armorSets.begin(), armorSets.end(), [=](const BaseArmorSet& as) { return !as.name.compare(name); });
            return it != armorSets.end() ? &*it : nullptr;
        }

        BaseArmorSet* GetArmorSetFor(RE::TESBoundObject* obj) {
            auto it = mapObjToSet.find(obj);
            if (it != mapObjToSet.end()) return it->second;
            return nullptr;
        }

        std::set<const RE::TESFile*> blacklist;
        std::set<RE::BGSKeyword*> kwSet;
        std::set<RE::BGSKeyword*> kwSlotSpecSet;
        std::set<RE::BGSKeyword*> kwFFSet;

        std::set<RE::BGSKeyword*> kwSetWeap;
        std::set<RE::BGSKeyword*> kwSetWeapTypes;

        std::vector<std::pair<std::string, RebalanceCurve>> curves;
        std::vector<BaseArmorSet> armorSets;
        std::unordered_map<RE::TESBoundObject*, BaseArmorSet*> mapObjToSet;

        std::set<std::string> lootProfiles;

        WordSet wordsDynamicVariants;
        WordSet wordsStaticVariants;
        WordSet wordsEitherVariants;
        WordSet wordsPieces;
        WordSet wordsDescriptive;

        WordSet wordsAllVariants;

        std::map<std::string, DynamicVariant> mapDynamicVariants;

        struct PreferenceVariants {
            std::size_t hash;
            int pref = Pref_Ignore;
        };

        std::map<std::string, PreferenceVariants> mapPrefVariants;

        std::unordered_map<RE::BGSKeyword*, CustomKeyword> mapCustomKWs;
        std::map<std::string, std::vector<RE::BGSKeyword*>> mapCustomKWTabs;

        std::map<RE::EnchantmentItem*, EnchantmentRanks> mapEnchantments;
        std::map<std::size_t, EnchantmentPool> mapEnchPools;

        std::set<const RE::TESForm*> recipeConditionBlacklist;

        void RebuildDisabledWords();

        std::vector<std::string> lsDisableWords;
        WordSet wordsAutoDisable;

        unsigned int usedSlotsMask = 0;
        ArmorChangeData acData;
        ArmorChangeParams acParams{acData};

        std::string strCriticalError = "Not loaded";

        int nFontSize = 13;

        bool bCloseConsole = true;
        bool bAutoDeleteGiven = false;
        bool bResetSliders = false;
        bool bRoundWeight = false;
        bool bHighlights = true;
        bool bNormalizeModDrops = true;
        bool bDisableCraftingRecipesOnRarity = false;
        bool bKeepCraftingBooks = false;
        bool bEnableRarityNullLoot = false;
        bool bResetSlotRemap = true;
        bool bEnableAllItems = false;
        bool bAllowInvalidRemap = false;
        bool bUseSecondaryRecipes = true;
        bool bEnableSmeltingRecipes = false;
        bool bEnableSkyrimWarmthHook = true;
        bool bShowFrostfallCoverage = false;
        bool bEnableProtectedSlotRemapping = false;
        bool bEnableArmorSlotModelFixHook = true;
        bool bPreventDistributionOfDynamicVariants = true;
        bool bEnableConsoleHook = true;
        bool bPauseWhileOpen = true;
        bool bShowAllRecipeConditions = false;

        bool bShortcutEscCloseWindow = true;

        bool bEnableEnchantmentDistrib = false;
        bool bEnchantRandomCharge = true;

        bool bExportUntranslated = false;

        bool bEnableDAVExports = true;
        bool bEnableDAVExportsAlways = false;

        bool bEnableBOSDetect = true;
        bool bEnableBOSFromGeneric = true;
        bool bEnableBOSFromReference = true;
        bool bEnableBOSFromConditional = true;

        bool bShowKeywordSlots = true;
        bool bReorderKeywordsForRelevance = true;
        bool bEquipPreviewForKeywords = true;

        bool isFrostfallInstalled = false;

        float fDropRates = 100.0f;
        float fEnchantRates = 100.0f;
        int verbosity = spdlog::level::info;
        int levelGranularity = 3;
        int craftingRarityMax = 2;

        int levelMaxDist = 1;
        int levelEnchDelay = 3;
        float enchChanceBase = 0.1f;
        float enchChanceBonus = 0.01f;
        float enchChanceBonusMax = 0.15f;
        int enchWeapChargeMin = 500;
        int enchWeapChargeMax = 3000;

        int flatArmorMod = 100;
        int flatValueMod = 1000;
        int flatWeapDamageMod = 30;
        int flatWeightMod = 60;

        float fTemperGoldCostRatio = 20.0f;
        float fCraftGoldCostRatio = 70.0f;

        Permissions permLocal;
        Permissions permShared;

        ArmorSlots slotsWillChange = 0;

        struct {
            RE::BGSKeyword* enable;
            RE::BGSKeyword* ignore;
            RE::BGSKeyword* warmth[5];
            RE::BGSKeyword* coverage[5];
        } ffKeywords;
    };

    extern Config g_Config;

    void ImportKeywords(const RE::TESFile* mod, const char* tab, const std::set<RE::BGSKeyword*>& kws);

}