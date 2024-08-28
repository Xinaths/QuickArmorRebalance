#pragma once

namespace QuickArmorRebalance
{
    template <class T>
    T* FindIn(const RE::TESFile* mod, const char* str)
    {
        if (!strncmp(str, "0x", 2)) {
            RE::FormID id = GetFullId(mod, (RE::FormID)strtol(str + 2, nullptr, 16));
            return RE::TESForm::LookupByID<T>(id);
        }
        if (isdigit(*str)) {
            RE::FormID id = GetFullId(mod, (RE::FormID)strtol(str + 2, nullptr,10));
            return RE::TESForm::LookupByID<T>(id);        
        }

        auto r = RE::TESForm::LookupByEditorID<T>(str);
        if (!r || r->GetFile(0) != mod) return nullptr;
        return r;
    }

	struct ModData
	{
        ModData(RE::TESFile* mod)
            : mod(mod)
			{}

        RE::TESFile* mod;
        std::set<RE::TESObjectARMO*> armors;
	};

    struct LootDistGroup
    {
        std::string name;
        int level = -1;
        int early = 0;
        int peak = 0;
        int falloff = 0;
        int minw = 1;
        int maxw = 5;
    };

    struct ContainerChance {
        int count;
        int chance;
    };

    using ArmorSet = std::vector<RE::TESObjectARMO*>;

    struct LootContainerGroup {
        std::map<RE::TESForm*, ContainerChance> large;
        std::map<RE::TESForm*, ContainerChance> small;

        std::map<LootDistGroup*, std::vector<RE::TESObjectARMO*>[3]> pieces;
        std::map<LootDistGroup*, std::vector<const ArmorSet*>[3]> sets;
    };

    struct LootDistProfile
    {
        std::set<LootContainerGroup*> containerGroups;
    };

    struct ItemDistData
    {
        LootDistProfile* profile;
        LootDistGroup* group;
        int rarity;

        RE::TESObjectARMO* piece;
        ArmorSet set;
    };

    struct ModLootData
    {
        std::map<const ArmorSet*, RE::TESBoundObject*> setList;

        std::map<std::string, LootContainerGroup> containerGroups;
        std::map<std::string, LootDistProfile> distProfiles;

        std::map<RE::TESObjectARMO*, ItemDistData> mapItemDist;
    };

	struct ProcessedData
	{

        std::map<RE::TESFile*, std::unique_ptr<ModData>> modData;
        std::vector<ModData*> sortedMods;
        std::set<const RE::TESFile*> modifiedFiles;
        std::set<const RE::TESFile*> modifiedFilesShared;
        std::set<const RE::TESFile*> modifiedFilesDeleted;

		std::set<RE::TESObjectARMO*> modifiedItems;
        std::set<RE::TESObjectARMO*> modifiedItemsShared;
        std::map<RE::TESObjectARMO*, RE::BGSConstructibleObject*> temperRecipe;
        std::map<RE::TESObjectARMO*, RE::BGSConstructibleObject*> craftRecipe;

        std::unique_ptr<ModLootData> loot;
        std::map<std::string, LootDistGroup> distGroups;

    };

    bool IsValidArmor(RE::TESObjectARMO*);

	void ProcessData();
    void LoadChangesFromFiles();

    void DeleteAllChanges(RE::TESFile* mod);

	extern ProcessedData g_Data;
}