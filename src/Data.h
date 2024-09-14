#pragma once

namespace QuickArmorRebalance
{
    using ArmorSlot = unsigned int;
    using ArmorSlots = unsigned int;

    static auto MapFindOr(const auto& map, const auto& val, const auto r) {
        auto it = map.find(val);
        if (it == map.end())
            return r;
        else
            return it->second;
    }

    static RE::TESForm* FindIn(const RE::TESFile* mod, const char* str, bool* pOtherFile = nullptr) {
        if (pOtherFile) *pOtherFile = false;
        if (!strncmp(str, "0x", 2)) {
            RE::FormID id = GetFullId(mod, (RE::FormID)strtol(str + 2, nullptr, 16));
            return RE::TESForm::LookupByID(id);
        }
        if (isdigit(*str)) {
            RE::FormID id = GetFullId(mod, (RE::FormID)strtol(str, nullptr,10));
            return RE::TESForm::LookupByID(id);        
        }

        if (auto pos = strchr(str, ':')) {
            if (pOtherFile) *pOtherFile = true;
            std::string fileName(str, pos - str);
            if (auto mod2 = RE::TESDataHandler::GetSingleton()->LookupModByName(fileName)) {
                return FindIn(mod2, pos + 1);
            }
            return nullptr;
        }

        auto r = RE::TESForm::LookupByEditorID(str);
        if (!r || r->GetFile(0) != mod) return nullptr;
        return r;
    }

    template <class T>
    T* FindIn(const RE::TESFile* mod, const char* str)
    {
        if (auto r = FindIn(mod, str)) return r->As<T>();
        return nullptr;
    }

	struct ModData
	{
        ModData(RE::TESFile* mod)
            : mod(mod)
			{}

        RE::TESFile* mod;
        std::set<RE::TESBoundObject*> items;
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
        std::map<RE::TESForm*, ContainerChance> weapon;

        std::map<LootDistGroup*, std::vector<RE::TESBoundObject*>[3]> pieces;
        std::map<LootDistGroup*, std::vector<const ArmorSet*>[3]> sets;
        std::map<LootDistGroup*, std::vector<RE::TESBoundObject*>[3]> weapons;
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

        RE::TESBoundObject* piece;
        ArmorSet set;
    };

    struct ModLootData
    {
        std::map<const ArmorSet*, RE::TESBoundObject*> setList;

        std::map<std::string, LootContainerGroup> containerGroups;
        std::map<std::string, LootDistProfile> distProfiles;

        std::map<RE::TESBoundObject*, ItemDistData> mapItemDist;
    };

	struct ProcessedData
	{

        std::map<RE::TESFile*, std::unique_ptr<ModData>> modData;
        std::vector<ModData*> sortedMods;
        std::set<const RE::TESFile*> modifiedFiles;
        std::set<const RE::TESFile*> modifiedFilesShared;
        std::set<const RE::TESFile*> modifiedFilesDeleted;

		std::set<RE::TESBoundObject*> modifiedItems;
        std::set<RE::TESBoundObject*> modifiedItemsShared;
        std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*> temperRecipe;
        std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*> craftRecipe;
        std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*> smeltRecipe;

        std::map<RE::TESObjectARMO*, ArmorSlots> modifiedArmorSlots;
        std::map<RE::TESObjectARMO*, float> modifiedWarmth;

        std::unique_ptr<ModLootData> loot;
        std::map<std::string, LootDistGroup> distGroups;

    };

    bool IsValidItem(RE::TESBoundObject* i);

	void ProcessData();
    void LoadChangesFromFiles();

    void DeleteAllChanges(RE::TESFile* mod);

	extern ProcessedData g_Data;
}