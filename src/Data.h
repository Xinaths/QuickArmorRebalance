#pragma once

std::string& toLowerUTF8(std::string& utf8_str);  // In NameParsing.cpp

namespace QuickArmorRebalance {
    using ArmorSet = std::vector<RE::TESObjectARMO*>;
    using ArmorSlot = unsigned int;
    using ArmorSlots = unsigned int;
    using WordSet = std::set<std::size_t>;

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
        auto it = map.find(val);
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
        if (isdigit(*str)) {
            RE::FormID id = GetFullId(mod, (RE::FormID)strtol(str, nullptr, 10));
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

    struct LootContainerGroup {
        std::map<RE::TESForm*, ContainerChance> large;
        std::map<RE::TESForm*, ContainerChance> small;
        std::map<RE::TESForm*, ContainerChance> weapon;

        std::map<LootDistGroup*, std::vector<RE::TESBoundObject*>[3]> pieces;
        std::map<LootDistGroup*, std::vector<const ArmorSet*>[3]> sets;
        std::map<LootDistGroup*, std::vector<RE::TESBoundObject*>[3]> weapons;

        EnchantProbability ench;
        bool bLeveled = true;
    };

    struct LootDistProfile {
        std::set<LootContainerGroup*> containerGroups;
    };

    struct ItemDistData {
        LootDistProfile* profile;
        LootDistGroup* group;
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
    };

    struct KeywordChanges {
        std::unordered_set<RE::TESBoundObject*> add;
        std::unordered_set<RE::TESBoundObject*> remove;
    };

    using KeywordChangeMap = std::unordered_map<RE::BGSKeyword*, KeywordChanges>;

    struct EnchantmentPool {
        std::string name;
        std::string strContents;
        std::map<RE::EnchantmentItem*, float> enchs;
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
        std::unordered_set<const RE::TESFile*> modifiedFiles;
        std::unordered_set<const RE::TESFile*> modifiedFilesShared;
        std::unordered_set<const RE::TESFile*> modifiedFilesDeleted;

        std::unordered_set<RE::TESBoundObject*> modifiedItems;
        std::unordered_set<RE::TESBoundObject*> modifiedItemsShared;
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

        std::unordered_map<RE::TESContainer*, EnchantProbability> distContainers;
        std::unordered_set<RE::TESBoundObject*> distItems;

        std::unordered_map<RE::TESBoundObject*, ObjEnchantParams> enchParams;
    };

    bool IsValidItem(RE::TESBoundObject* i);

    void ProcessData();
    void LoadChangesFromFiles();

    void ForChangesInFolder(const char* sub, const std::function<void(const RE::TESFile*, std::filesystem::path)> fn);

    void DeleteAllChanges(RE::TESFile* mod);

    extern ProcessedData g_Data;
}