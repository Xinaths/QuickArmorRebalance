#pragma once

std::string& toLowerUTF8(std::string& utf8_str);  // In NameParsing.cpp

namespace QuickArmorRebalance {
    using ArmorSet = std::vector<RE::TESObjectARMO*>;
    using ArmorSlot = unsigned int;
    using ArmorSlots = unsigned int;
    using WordSet = std::set<std::size_t>;

    struct Region;

    enum ERegionRarity { eRegion_Same, eRegion_Common, eRegion_Uncommon, eRegion_Rare, eRegion_Exotic, eRegion_RarityCount };
    enum ELootType { eLoot_Set, eLoot_Armor, eLoot_Weapon, eLoot_TypeCount};

    enum EItemChanges { 
        eChange_Stats       = (1 << 0), 
        eChange_Slots       = (1 << 1), 
        eChange_Keywords    = (1 << 2), 
        eChange_Loot        = (1 << 3),
        eChange_Survival    = (1 << 4),
        eChange_Conversion  = (1 << 5),
        eChange_Recipes     = (1 << 6),
        eChange_Region      = (1 << 7),
    };

    struct DynamicVariant {
        std::string name;
        WordSet hints;
        WordSet autos;

        struct {
            bool perSlot = false;
            std::string display;
            std::string variant;
        } DAV;
    };

    using VariantSetMap = std::map<std::size_t, ArmorSet>;
    using DynamicVariantSets = std::map<const DynamicVariant*, VariantSetMap>;

    bool ReadJSONFile(std::filesystem::path path, rapidjson::Document& doc, bool bEditing = true);
    bool WriteJSONFile(std::filesystem::path path, rapidjson::Document& doc);

    inline int GetJsonBool(const rapidjson::Value& parent, const char* id, bool d = false) {
        if (parent.HasMember(id)) {
            const auto& v = parent[id];
            if (v.IsBool()) return v.GetBool();
        }

        return d;
    }

    inline int GetJsonInt(const rapidjson::Value& parent, const char* id, int min = 0, int max = 0, int d = 0) {
        if (parent.HasMember(id)) {
            const auto& v = parent[id];
            if (v.IsInt()) return std::clamp(v.GetInt(), min, max);
        }

        return std::max(min, d);
    }

    inline float GetJsonFloat(const rapidjson::Value& parent, const char* id, float min = 0.0f, float max = 0.0f, float d = 0.0f) {
        if (parent.HasMember(id)) {
            const auto& v = parent[id];
            if (v.IsNumber()) return std::clamp(v.GetFloat(), min, max);
        }

        return std::max(min, d);
    }

    inline void ToLower(std::string& str) {
        // std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c, std::locale()); });
        toLowerUTF8(str);
    }

    inline std::string MakeLower(std::string str) {
        ToLower(str);
        return str;
    }

    inline std::string MakeLower(const char* str) { return MakeLower(std::string(str)); }

    static auto MapFindOr(const auto& map, const auto& val, const auto r) {
        const auto& it = map.find(val);
        if (it == map.end())
            return r;
        else
            return it->second;
    }

    template <typename MapType>
    static auto MapFindOrNull(const MapType& map, const typename MapType::key_type& val) -> const typename MapType::mapped_type {
        auto it = map.find(val);
        if (it == map.end())
            return nullptr;
        else
            return it->second;
    }

    template <typename MapType>
    auto MapFind(const MapType& map, const typename MapType::key_type& key) -> const typename MapType::mapped_type* {
        auto it = map.find(key);
        if (it != map.end()) {
            return &(it->second);  // Return a pointer to the value
        }
        return nullptr;  // Return nullptr if not found
    }

    template <typename MapType>
    bool MapFind(const MapType& map, const typename MapType::key_type& key, typename MapType::mapped_type& ret) {
        auto it = map.find(key);
        if (it != map.end()) {
            ret = it->second;
            return true;
        }
        return false; 
    }


    inline rapidjson::Value& EnsureHas(rapidjson::Value& obj, const char* field, rapidjson::Type t, rapidjson::MemoryPoolAllocator<>& al) {
        if (!obj.HasMember(field) || obj[field].GetType() != t) {
            obj.RemoveMember(field);
            obj.AddMember(rapidjson::Value(field, al), rapidjson::Value(t), al);
        }
        return obj[field];
    }

    static RE::TESForm* FindIn(const RE::TESFile* mod, const char* str, bool* pOtherFile = nullptr, bool picky = false) {
        if (pOtherFile) *pOtherFile = false;
        if (!strncmp(str, "0x", 2)) {
            RE::FormID id = GetFullId(mod, (RE::FormID)strtol(str + 2, nullptr, 16));
            return RE::TESForm::LookupByID(id);
        }

        // Has to come before isdigit because some mods might have numeric names so "5Armors.esp:0x123" would fail
        if (auto pos = strchr(str, ':')) {
            if (pOtherFile) *pOtherFile = true;
            std::string fileName(str, pos - str);
            if (auto mod2 = RE::TESDataHandler::GetSingleton()->LookupModByName(fileName)) {
                return FindIn(mod2, pos + 1);
            }
            return nullptr;
        }

        // Can EditorId's start with digits?
        if (isdigit(*str)) {
            RE::FormID id = GetFullId(mod, (RE::FormID)strtol(str, nullptr, 10));
            return RE::TESForm::LookupByID(id);
        }

        auto r = RE::TESForm::LookupByEditorID(str);
        if (!r || (picky && r->GetFile(0) != mod)) return nullptr;
        return r;
    }

    template <class T>
    T* FindIn(const RE::TESFile* mod, const char* str, bool picky = true) {
        if (auto r = FindIn(mod, str, nullptr, picky)) return r->As<T>();
        return nullptr;
    }

    inline bool IsSingleSlot(ArmorSlots slots) { return (slots & (slots - 1)) == 0; }

    inline int GetSlotIndex(ArmorSlots slots) {
        unsigned long slot = 0;
        _BitScanForward(&slot, slots);
        return (int)slot;
    }

    struct ModData {
        ModData(RE::TESFile* mod) : mod(mod) {}

        RE::TESFile* mod;
        std::set<RE::TESBoundObject*> items;

        bool bModified = false;
        bool bHasDynamicVariants = false;
        bool bHasPotentialDVs = false;
        unsigned int changes = 0;
    };

    struct LootDistGroup {
        std::string name;
        int level = -1;
        int early = 0;
        int peak = 0;
        int falloff = 0;
        int minw = 1;
        int maxw = 5;
    };

    struct EnchantProbability {
        float enchRate = 1.0f;
        float enchPower = 1.0f;

        bool IsDefault() const { return enchRate == 1.0f && enchPower == 1.0f; }
        bool operator==(const EnchantProbability& other) { return enchRate == other.enchRate && enchPower == other.enchPower; }
    };

    struct ContainerChance {
        int count = 1;
        int chance = 100;
        EnchantProbability ench;
    };

    template <class TYPE, int COUNT>
    class SplitSets {
    public:
        std::set<TYPE*>& operator[](int n) { return set[n]; }

        void Remove(TYPE* region) {
            for (int i = 0; i < COUNT; i++) set->erase(region);
        }
        void Add(TYPE* other, int nGroup) {
            Remove(other);
            if (nGroup < COUNT) set[nGroup].insert(other);
        }

    protected:
        std::set<TYPE*> set[COUNT];
    };

    struct LootContainerGroup {
        using ContainerChanceMap = std::map<RE::TESForm*, ContainerChance>;

        std::map<Region*, ContainerChanceMap> large;
        std::map<Region*, ContainerChanceMap> small;
        std::map<Region*, ContainerChanceMap> weapon;

        struct Items {
            std::vector<RE::TESBoundObject*> pieces;
            std::vector<const ArmorSet*> sets;
            std::vector<RE::TESBoundObject*> weapons;
        };

        using Rarities = Items[3];
        using Tiers = std::map<LootDistGroup*, Rarities>;
        using Regions = std::map<const Region*, Tiers>;

        Regions contents;

        std::set<Region*> regions;
        SplitSets<LootContainerGroup, eRegion_RarityCount> migration;

        EnchantProbability ench;

        std::string name;
        bool bLeveled = true;
    };

    struct LootDistProfile {
        std::set<LootContainerGroup*> containerGroups;
    };

    struct ItemDistData {
        LootDistProfile* profile;
        LootDistGroup* group;
        const Region* region;
        int rarity;

        RE::TESBoundObject* piece;
        ArmorSet set;
    };

    struct ModLootData {
        std::map<const ArmorSet*, RE::TESBoundObject*> setList;

        std::map<std::string, LootContainerGroup> containerGroups;
        std::map<std::string, LootDistProfile> distProfiles;

        std::map<RE::TESBoundObject*, ItemDistData> mapItemDist;

        std::unordered_set<RE::TESObjectARMA*> dynamicVariantsDAV;
        std::map<std::size_t, std::unordered_set<RE::TESObjectARMO*>> prefVartWith;
        std::map<std::size_t, std::unordered_set<RE::TESObjectARMO*>> prefVartWithout;

        std::unordered_map<RE::TESForm*, std::set<RE::TESForm*>> mapContainerCopy;

        std::unordered_map<const void*, RE::TESBoundObject*> cacheGroupList;
        std::unordered_map<const void*, RE::TESBoundObject*> cacheRegionalGroupTierList[eLoot_TypeCount];
        std::map<LootDistGroup*, std::map<const LootContainerGroup*, std::map<Region*, RE::TESBoundObject*>>> cacheSourceSelectionList[eLoot_TypeCount];
        std::map<const LootContainerGroup*, std::map<Region*, RE::TESBoundObject*[3]>> cacheLowerTierItems[eLoot_TypeCount];

        //std::map<Region*, std::map<LootContainerGroup*, RE::TESBoundObject*>> tblRegionSourceCurveCache[eLoot_TypeCount];
    };

    struct KeywordChanges {
        std::unordered_set<RE::TESBoundObject*> add;
        std::unordered_set<RE::TESBoundObject*> remove;
    };

    using KeywordChangeMap = std::unordered_map<RE::BGSKeyword*, KeywordChanges>;
    using WeightedEnchantments = std::map<RE::EnchantmentItem*, float>;

    struct EnchantmentPool {
        std::string name;
        std::string strContents;
        WeightedEnchantments enchs;
    };

    struct EnchantParams : public EnchantProbability {
        const EnchantmentPool* enchPool = nullptr;
    };

    struct EnchantmentRanks {
        std::vector<RE::EnchantmentItem*> ranks;
        int levelMin = 1;
        int levelMax = INT_MAX;
    };

    struct ObjEnchantParams {
        EnchantParams base;
        EnchantParams unique;
        int level = 1;
        float uniquePoolChance = 0.5f;
    };

    struct ProcessedData {
        std::map<const RE::TESFile*, std::unique_ptr<ModData>> modData;
        std::vector<ModData*> sortedMods;
        std::unordered_map<const RE::TESFile*, unsigned int> modifiedFiles;
        std::unordered_map<const RE::TESFile*, unsigned int> modifiedFilesShared;
        std::unordered_set<const RE::TESFile*> modifiedFilesDeleted;

        std::unordered_map<RE::TESBoundObject*, unsigned int> modifiedItems;
        std::unordered_map<RE::TESBoundObject*, unsigned int> modifiedItemsShared;
        std::unordered_set<RE::TESBoundObject*> modifiedItemsDeleted;
        std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*> temperRecipe;
        std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*> craftRecipe;
        std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*> smeltRecipe;

        std::map<RE::TESObjectARMO*, ArmorSlots> modifiedArmorSlots;
        std::map<RE::TESObjectARMO*, float> modifiedWarmth;

        std::unordered_map<size_t, ArmorSlots> remapFileArmorSlots;
        std::unordered_set<size_t> noModifyModels;

        std::unique_ptr<ModLootData> loot;
        std::map<std::string, LootDistGroup> distGroups;
        std::vector<LootDistGroup*> distGroupsSorted;

        std::unordered_map<RE::TESContainer*, EnchantProbability> distContainers;
        std::unordered_set<RE::TESBoundObject*> distItems;

        std::unordered_map<RE::TESBoundObject*, ObjEnchantParams> enchParams;
        std::unordered_map<RE::TESBoundObject*, WeightedEnchantments*> staffEnchGroup;

        std::vector<RE::TESForm*> recipeConditions;
    };

    bool IsValidItem(RE::TESBoundObject* i);

    void ProcessData();
    void LoadChangesFromFiles();

    void ForChangesInFolder(const char* sub, const std::function<void(const RE::TESFile*, std::filesystem::path)> fn);

    void DeleteAllChanges(RE::TESFile* mod);

    extern ProcessedData g_Data;
}