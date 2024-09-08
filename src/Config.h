#pragma once

#include "Data.h"

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

        ArmorSlots GetSlots() const { return 1 << (slot - 30); }

        ArmorSlot slot = 0;
        int weight = 0;
        Tree children;
    };

    struct BaseArmorSet {
        std::string name;
        std::string strContents;
        LootDistGroup* loot = nullptr;
        std::set<RE::TESObjectARMO*> items;
        std::set<RE::TESObjectWEAP*> weaps;
        std::set<RE::TESAmmo*> ammo;

        RE::TESObjectWEAP* FindMatching(RE::TESObjectWEAP* w) const;
        RE::TESAmmo* FindMatching(RE::TESAmmo* w) const;
    };

    struct ArmorChangeParams {
        std::vector<RE::TESBoundObject*> filteredItems;
        std::vector<RE::TESBoundObject*> items;

        BaseArmorSet* armorSet = nullptr;
        RebalanceCurveNode::Tree* curve = nullptr;

        struct SliderPair {
            bool bModify = true;
            float fScale = 100.0f;
        };

        bool isWornArmor = false;

        bool bModifyKeywords = true;

        struct {
            SliderPair rating;
            SliderPair weight;
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
        bool bModifyWeapDamage = true;
        bool bModifyWeapWeight = true;
        bool bModifyWeapSpeed = true;
        bool bModifyWeapStagger = true;

        RecipePermissions crafting;
        RecipePermissions temper;

    };

    struct Config {
        bool Load();
        bool LoadFile(std::filesystem::path path);

        void Save();

        void AddUserBlacklist(RE::TESFile* mod);

        std::set<const RE::TESFile*> blacklist;
        std::set<RE::BGSKeyword*> kwSet;
        std::set<RE::BGSKeyword*> kwSlotSpecSet;

        std::set<RE::BGSKeyword*> kwSetWeap;
        std::set<RE::BGSKeyword*> kwSetWeapTypes;

        std::vector<std::pair<std::string, RebalanceCurveNode::Tree>> curves;
        std::vector<BaseArmorSet> armorSets;
        std::set<std::string> lootProfiles;

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

        float fDropRates = 100.0f;
        int verbosity = spdlog::level::info;
        int levelGranularity = 3;
        int craftingRarityMax = 2;

        Permissions permLocal;
        Permissions permShared;

        ArmorSlots slotsWillChange = 0;
    };

    extern Config g_Config;
}