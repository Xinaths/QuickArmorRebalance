#pragma once

#include "Data.h"
#include "ArmorSetBuilder.h"

#define PATH_ROOT "Data/SKSE/Plugins/" PLUGIN_NAME "/"
#define PATH_CONFIGS "config/"
#define PATH_CHANGES "changes/"
#define PATH_CUSTOMKEYWORDS "custom keywords/"

namespace QuickArmorRebalance {  

    enum Preference {
        Pref_Ignore,
        Pref_With,
        Pref_Without
    };
   
    struct LootDistGroup;

    const unsigned int kCosmeticSlotMask = 0;
    //(unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHead | (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHair;

    const ArmorSlots kHeadSlotMask = (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead |
                                     (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHair |
                                     (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kCirclet;

    const ArmorSlots kProtectedSlotMask =
        (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kBody | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHands |
        (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kFeet | (ArmorSlots)RE::BIPED_MODEL::BipedObjectSlot::kHead;

    struct RebalanceCurveNode {
        using Tree = std::vector<RebalanceCurveNode>;

        ArmorSlots GetSlots() const { return 1 << (slot - 30); }

        ArmorSlot slot = 0;
        int weight = 0;
        Tree children;
    };

    struct BaseArmorSet {
        std::string name;
        std::string strContents;
        std::string strFallbackRecipeSet;
        LootDistGroup* loot = nullptr;
        std::set<RE::TESObjectARMO*> items;
        std::set<RE::TESObjectWEAP*> weaps;
        std::set<RE::TESAmmo*> ammo;

        RE::TESObjectARMO* FindMatching(RE::TESObjectARMO* w) const;
        RE::TESObjectWEAP* FindMatching(RE::TESObjectWEAP* w) const;
        RE::TESAmmo* FindMatching(RE::TESAmmo* w) const;
    };

    struct ArmorChangeParams {
        std::vector<RE::TESBoundObject*> filteredItems;
        std::vector<RE::TESBoundObject*> items;

        BaseArmorSet* armorSet = nullptr;
        RebalanceCurveNode::Tree* curve = nullptr;

        struct SliderPair {
            float fScale = 100.0f;
            bool bModify = true;
        };

        bool isWornArmor = false;

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

        SliderPair value;

        struct RecipeOptions {
            bool bModify = true;
            bool bNew = false;
            bool bFree = false;
        };

        RecipeOptions temper;
        RecipeOptions craft;

        const char* distProfile = nullptr;
        int rarity = 0;
        bool bDistribute = false;
        bool bDistAsSet = true;
        bool bDistAsPieces = true;
        bool bMatchSetPieces = false;
        bool bMerge = true;

        mutable ArmorSlots remapMask = 0;
        std::map<int, int> mapArmorSlots;

        DynamicVariantSets dvSets;
        AnalyzeResults analyzeResults;

        KeywordChangeMap mapKeywordChanges;

        mutable bool bMixedSetDone = false;
    };

    struct Permissions {
        struct RecipePermissions {
            bool bModify = true;
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
            auto it = std::find_if(armorSets.begin(), armorSets.end(),
                                   [=](const BaseArmorSet& as) { return !as.name.compare(name); });
            return it != armorSets.end() ? &*it : nullptr;
        }

        std::set<const RE::TESFile*> blacklist;
        std::set<RE::BGSKeyword*> kwSet;
        std::set<RE::BGSKeyword*> kwSlotSpecSet;
        std::set<RE::BGSKeyword*> kwFFSet;

        std::set<RE::BGSKeyword*> kwSetWeap;
        std::set<RE::BGSKeyword*> kwSetWeapTypes;

        std::vector<std::pair<std::string, RebalanceCurveNode::Tree>> curves;
        std::vector<BaseArmorSet> armorSets;
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

        unsigned int usedSlotsMask = 0;
        ArmorChangeParams acParams;

        std::string strCriticalError = "Not loaded";

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

        bool bEnableDAVExports = true;
        bool bEnableDAVExportsAlways = false;

        bool bShowKeywordSlots = true;
        bool bReorderKeywordsForRelevance = true;
        bool bEquipPreviewForKeywords = true;

        bool isFrostfallInstalled = false;

        float fDropRates = 100.0f;
        int verbosity = spdlog::level::info;
        int levelGranularity = 3;
        int craftingRarityMax = 2;

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