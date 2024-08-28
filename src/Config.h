#pragma once

#define PATH_ROOT "Data/SKSE/Plugins/" PLUGIN_NAME "/"
#define PATH_CONFIGS "config/"
#define PATH_CHANGES "changes/"

namespace QuickArmorRebalance {

    struct LootDistGroup;

    const unsigned int kCosmeticSlotMask = 0;
    //(unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHead | (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHair;

    const unsigned int kHeadSlotMask = (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHead |
                                       (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kHair |
                                       (unsigned int)RE::BIPED_MODEL::BipedObjectSlot::kCirclet;

    struct RebalanceCurveNode {
        using Tree = std::vector<RebalanceCurveNode>;

        int slot = 0;
        int weight = 0;
        Tree children;
    };

    struct BaseArmorSet {
        std::string name;
        LootDistGroup* loot = nullptr;
        std::set<RE::TESObjectARMO*> items;
    };

    struct ArmorChangeParams {
        std::vector<RE::TESObjectARMO*> items;

        BaseArmorSet* armorSet = nullptr;
        RebalanceCurveNode::Tree* curve = nullptr;

        bool isWornArmor = false;
        bool bModifyKeywords = true;
        bool bModifyArmor = true;
        float fArmorScale = 100.0f;
        bool bModifyValue = true;
        float fValueScale = 100.0f;
        bool bModifyWeight = true;
        float fWeightScale = 100.0f;

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
        bool bModifyArmorRating = true;
        bool bModifyValue = true;
        bool bModifyWeight = true;

        RecipePermissions crafting;
        RecipePermissions temper;

    };

    struct Config {
        bool Load();
        bool LoadFile(std::filesystem::path path);

        void Save();

        std::set<const RE::TESFile*> blacklist;
        std::set<RE::BGSKeyword*> kwSet;
        std::set<RE::BGSKeyword*> kwSlotSpecSet;

        std::vector<std::pair<std::string, RebalanceCurveNode::Tree>> curves;
        std::vector<BaseArmorSet> armorSets;
        std::set<std::string> lootProfiles;

        unsigned int usedSlotsMask = 0;
        ArmorChangeParams acParams;

        bool bValid = false;

        bool bCloseConsole = true;
        bool bAutoDeleteGiven = false;
        bool bResetSliders = false;
        bool bRoundWeight = false;
        bool bHighlights = true;
        bool bNormalizeModDrops = true;
        bool bDisableCraftingRecipesOnRarity = false;
        bool bKeepCraftingBooks = false;

        float fDropRates = 100.0f;
        int verbosity = spdlog::level::info;
        int levelGranularity = 3;
        int craftingRarityMax = 2;

        Permissions permLocal;
        Permissions permShared;
    };

    extern Config g_Config;
}