#include "UI.h"

#include "ArmorChanger.h"
#include "ArmorSetBuilder.h"
#include "Config.h"
#include "Data.h"
#include "ImGui/imgui_impl_dx11.h"
#include "ImGuiIntegration.h"
#include "Localization.h"
#include "ModIntegrations.h"
// #include "ImGui/imgui_freetype.h"

#include "NameParsing.h"

using namespace QuickArmorRebalance;
constexpr auto LZ = QuickArmorRebalance::Localize;

constexpr int kItemListLimit = 2000;
constexpr int kItemApplyWarningThreshhold = 100;

/*
const char* strSlotDesc[] = {
    "Slot 30 - Head",       "Slot 31 - Hair",        "Slot 32 - Body",        "Slot 33 - Hands",      "Slot 34 - Forearms",  "Slot 35 - Amulet",  "Slot 36 - Ring",
    "Slot 37 - Feet",       "Slot 38 - Calves",      "Slot 39 - Shield",      "Slot 40 - Tail",       "Slot 41 - Long Hair", "Slot 42 - Circlet", "Slot 43 - Ears",
    "Slot 44 - Face",       "Slot 45 - Neck",        "Slot 46 - Chest",       "Slot 47 - Back",       "Slot 48 - ???",       "Slot 49 - Pelvis",  "Slot 50 - Decapitated Head",
    "Slot 51 - Decapitate", "Slot 52 - Lower body",  "Slot 53 - Leg (right)", "Slot 54 - Leg (left)", "Slot 55 - Face2",     "Slot 56 - Chest2",  "Slot 57 - Shoulder",
    "Slot 58 - Arm (left)", "Slot 59 - Arm (right)", "Slot 60 - ???",         "Slot 61 - ???",        "<REMOVE SLOT>"};
    */

static ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return {a.x + b.x, a.y + b.y}; }
static ImVec2 operator/(const ImVec2& a, int b) { return {a.x / b, a.y / b}; }

template <typename... Args>
std::string LZFormat(const char* rt_fmt_str, Args&&... args) {
    return std::vformat(LZ(rt_fmt_str), std::make_format_args(args...));
}

bool StringContainsI(const char* s1, const char* s2) {
    std::string str1(s1), str2(s2);
    ToLower(str1);
    ToLower(str2);
    return str1.contains(str2);
}

void MakeTooltip(const char* str, bool delay = false) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | (delay ? ImGuiHoveredFlags_DelayNormal : 0))) ImGui::SetTooltip(str);
}

bool MenuItemConfirmed(const char* str) {
    bool bRet = false;
    if (ImGui::BeginMenu(str)) {
        if (ImGui::Selectable("Yes")) bRet = true;
        ImGui::Selectable("No");
        ImGui::EndMenu();
    }
    return bRet;
}

std::vector<ModData*> GetFilteredMods(int nModFilter) {
    std::vector<ModData*> list;
    list.reserve(g_Data.sortedMods.size());
    switch (nModFilter) {
        case 0:
            list = g_Data.sortedMods;
            break;
        case 1:
            std::copy_if(g_Data.sortedMods.begin(), g_Data.sortedMods.end(), std::back_inserter(list), [](ModData* mod) { return !mod->bModified; });
            break;
        case 2:
            std::copy_if(g_Data.sortedMods.begin(), g_Data.sortedMods.end(), std::back_inserter(list), [](ModData* mod) { return mod->bModified; });
            break;
        case 3: {
            static bool bOnce = false;
            if (!bOnce) {
                bOnce = true;

                for (auto& i : g_Data.modData) {
                    if (i.second->bModified && !i.second->bHasDynamicVariants) {
                        AnalyzeResults results;
                        std::vector<RE::TESBoundObject*> items(i.second->items.begin(), i.second->items.end());
                        AnalyzeArmor(items, results);
                        i.second->bHasPotentialDVs = !results.sets[0].empty() || !results.sets[1].empty();
                    }
                }
            }
        }
            std::copy_if(g_Data.sortedMods.begin(), g_Data.sortedMods.end(), std::back_inserter(list),
                         [](ModData* mod) { return mod->bModified && !mod->bHasDynamicVariants && mod->bHasPotentialDVs; });
            break;
    }

    return list;
}

struct ItemFilter {
    char nameFilter[200]{""};

    enum TypeFilter { ItemType_All, ItemType_Armor, ItemType_Weapon, ItemType_Ammo };

    int nType = 0;

    enum SlotFilterMode { SlotsAny, SlotsAll, SlotsNot };

    int slotMode = SlotsAny;
    ArmorSlots slots = 0;

    bool bFilterChanged = true;
    bool bUnmodified = false;

    bool Pass(RE::TESBoundObject* obj) const {
        // Run these fastest to slowest
        if (nType) {
            switch (nType) {
                case ItemType_Armor:
                    if (!obj->As<RE::TESObjectARMO>()) return false;
                    break;
                case ItemType_Weapon:
                    if (!obj->As<RE::TESObjectWEAP>()) return false;
                    break;
                case ItemType_Ammo:
                    if (!obj->As<RE::TESAmmo>()) return false;
                    break;
            }
        }

        if (*nameFilter && !StringContainsI(obj->GetName(), nameFilter)) return false;

        if (slots) {
            if (auto armor = obj->As<RE::TESObjectARMO>()) {
                auto s = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
                switch (slotMode) {
                    case SlotsAny:
                        if ((s & slots) == 0) return false;
                        break;
                    case SlotsAll:
                        if ((s & slots) != slots) return false;
                        break;
                    case SlotsNot:
                        if ((s & slots) != 0) return false;
                        break;
                }
            } else
                return false;
        }

        if (bUnmodified && (g_Data.modifiedItems.contains(obj) || g_Data.modifiedItemsShared.contains(obj))) return false;

        return true;
    }
};

const char* RightAlign(const char* text, float extra = 0.0f) {
    float w = extra + ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
    return text;
}

struct GivenItems {
    void UnequipCurrent() {
        stored.clear();
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            for (auto& item : player->GetInventory()) {
                if (item.second.second->IsWorn() && item.first->IsArmor()) {
                    if (auto i = item.first->As<RE::TESObjectARMO>()) {
                        // if (((unsigned int)i->GetSlotMask() & g_Config.usedSlotsMask) == 0) {  // Not a slot we interact with - probably physics or such
                        //     continue;
                        // }

                        stored.insert(i);
                        RE::ActorEquipManager::GetSingleton()->UnequipObject(player, i, nullptr, 1, i->GetEquipSlot(), false, false, false);
                    }
                }
            }
        }
    }

    using ItemInventoryData = std::pair<RE::TESObjectREFR::Count, std::unique_ptr<RE::InventoryEntryData>>;

    bool FindInInventory(RE::TESBoundObject* item, ItemInventoryData* data = nullptr) {
        if (!item) return false;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        auto inv = player->GetInventory();  // Getting a copy, highly inefficient
        auto it = inv.find(item);
        if (it == inv.end()) return false;

        if (data) *data = std::move(it->second);
        return true;
    }

    void Unequip(RE::TESBoundObject* item) {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        if (auto armor = item->As<RE::TESObjectARMO>()) recentEquipSlots &= ~(ArmorSlots)armor->GetSlotMask();

        RE::ActorEquipManager::GetSingleton()->UnequipObject(player, item);
    }

    void Equip(RE::TESBoundObject* item) {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        if (auto armor = item->As<RE::TESObjectARMO>()) {
            auto slots = (unsigned int)armor->GetSlotMask();
            if ((slots & recentEquipSlots) == 0) {
                recentEquipSlots |= slots;

                // Not using AddTask will result in it not un-equipping current items
                SKSE::GetTaskInterface()->AddTask(
                    [=]() { RE::ActorEquipManager::GetSingleton()->EquipObject(player, item, nullptr, 1, armor->GetEquipSlot(), false, false, false); });
            }
        } else {
            SKSE::GetTaskInterface()->AddTask([=]() { RE::ActorEquipManager::GetSingleton()->EquipObject(player, item); });
        }
    }

    void Restore() {
        if (stored.empty()) return;

        SKSE::GetTaskInterface()->AddTask([=]() {
            auto manager = RE::ActorEquipManager::GetSingleton();
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;

            for (auto i : stored) {
                RE::BGSEquipSlot* equipSlot = nullptr;
                if (auto e = i->As<RE::BGSEquipType>()) equipSlot = e->GetEquipSlot();

                manager->EquipObject(player, i, nullptr, 1, equipSlot, false, false, false);
            }
            stored.clear();
        });
    }

    void Give(RE::TESBoundObject* item, bool equip = false, bool reuse = false) {
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes
        if (!item) return;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        if (!reuse || !FindInInventory(item)) {
            player->AddObjectToContainer(item, nullptr, 1, nullptr);
            items.push_back({ImGui::GetFrameCount(), item});
        }

        // After much testing, Skyrim seems to REALLY not like equipping more then one item per
        // frame And the per frame limit counts while the console is open. Doing so results in it
        // letting you equip as many items in one slot as you'd like So instead, have to mimic
        // the expected behavior and hopefully its good enough

        if (equip) Equip(item);
    }

    void ToggleEquip(RE::TESBoundObject* item) {
        // if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  //I think this is safe, only if we're not adding /
        // removing from the list
        if (!item) return;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        ItemInventoryData invEntry;
        if (FindInInventory(item, &invEntry)) {
            if (invEntry.second->IsWorn())
                Unequip(item);
            else
                Equip(item);

            return;
        }

        // Doesn't already have item

        Give(item, true);
    }

    void Remove(RE::TESBoundObject* item) {
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto it = std::find_if(items.begin(), items.end(), [=](auto& i) { return i.second == item; });
        if (it != items.end()) {
            player->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            items.erase(it);
        }
    }

    void Remove() {
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        for (auto i : items) player->RemoveItem(i.second, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);

        items.clear();
    }

    void Pop(bool unequip = false) {
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        if (items.empty()) return;
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto t = items.back().first;

        while (!items.empty() && items.back().first == t) {
            auto item = items.back().second;
            if (unequip) {
                if (auto armor = item->As<RE::TESObjectARMO>()) recentEquipSlots &= ~(ArmorSlots)armor->GetSlotMask();
                RE::ActorEquipManager::GetSingleton()->UnequipObject(player, item);
            }

            // AddTask isn't strictly needed, but other mods were crashing if an unequip was called first and the item was deleted
            SKSE::GetTaskInterface()->AddTask([player, item = items.back().second]() { player->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr); });
            items.pop_back();
        }
    }

    unsigned int recentEquipSlots = 0;
    std::vector<std::pair<int, RE::TESBoundObject*>> items;
    std::set<RE::TESBoundObject*> stored;
};

bool SliderTable() {
    if (ImGui::BeginTable("Slider Table", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("SliderCol1");
        ImGui::TableSetupColumn("SliderCol2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("SliderCol3");
        return true;
    }
    return false;
}

void SliderRow(const char* field, ArmorChangeParams::SliderPair& pair, float min = 0.0f, float max = 300.0f, float def = 100.0f) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(field);
    ImGui::Checkbox(LZFormat("Modify {}", field).c_str(), &pair.bModify);
    ImGui::BeginDisabled(!pair.bModify);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::SliderFloat("##Scale", &pair.fScale, min, max, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TableNextColumn();
    if (ImGui::Button(LZ("Reset"))) pair.fScale = def;
    ImGui::EndDisabled();
    ImGui::PopID();
}

void PermissionsChecklist(const char* id, Permissions& p) {
    ImGui::PushID(id);
    ImGui::Checkbox(LZ("Distribute loot"), &p.bDistributeLoot);
    ImGui::Checkbox(LZ("Modify keywords"), &p.bModifyKeywords);
    ImGui::Checkbox(LZ("Modify custom keywords"), &p.bModifyCustomKeywords);
    ImGui::Checkbox(LZ("Modify armor slots"), &p.bModifySlots);
    ImGui::Checkbox(LZ("Modify armor rating"), &p.bModifyArmorRating);
    ImGui::Checkbox(LZ("Modify armor weight"), &p.bModifyWeight);
    ImGui::Checkbox(LZ("Modify armor warmth"), &p.bModifyWarmth);
    ImGui::Checkbox(LZ("Modify weapon damage"), &p.bModifyWeapDamage);
    ImGui::Checkbox(LZ("Modify weapon weight"), &p.bModifyWeapWeight);
    ImGui::Checkbox(LZ("Modify weapon speed"), &p.bModifyWeapSpeed);
    ImGui::Checkbox(LZ("Modify weapon stagger"), &p.bModifyWeapStagger);
    ImGui::Checkbox(LZ("Modify value"), &p.bModifyValue);
    ImGui::Checkbox(LZ("Modify crafting recipes"), &p.crafting.bModify);
    ImGui::Checkbox(LZ("Create crafting recipes"), &p.crafting.bCreate);
    ImGui::Checkbox(LZ("Free crafting recipes"), &p.crafting.bFree);
    ImGui::Checkbox(LZ("Modify temper recipes"), &p.temper.bModify);
    ImGui::Checkbox(LZ("Create temper recipes"), &p.temper.bCreate);
    ImGui::Checkbox(LZ("Free temper recipes"), &p.temper.bFree);
    ImGui::PopID();
}

enum { ModSpecial_Worn, ModSpecial_All };

void AddFormsToList(const auto& all, const ItemFilter& filter) {
    auto& data = *g_Config.acParams.data;
    for (auto i : all) {
        if (!IsValidItem(i)) continue;
        if (!filter.Pass(i)) continue;

        if (data.filteredItems.size() < kItemListLimit) data.filteredItems.push_back(i);
    }
}

short g_filterRound = 0;

bool GetCurrentListItems(ModData* curMod, int nModSpecial, const ItemFilter& filter, AnalyzeResults& results) {
    static short filterRound = -1;
    if (filterRound == g_filterRound) return false;
    filterRound = g_filterRound;

    auto& data = *g_Config.acParams.data;
    data.filteredItems.clear();
    if (curMod) {
        for (auto i : curMod->items) {
            if (!filter.Pass(i)) continue;

            data.filteredItems.push_back(i);
        }
    } else {
        switch (nModSpecial) {
            case ModSpecial_Worn:
                if (auto player = RE::PlayerCharacter::GetSingleton()) {
                    for (auto& item : player->GetInventory()) {
                        if (item.second.second->IsWorn()) {
                            if (!IsValidItem(item.first)) continue;
                            if (!filter.Pass(item.first)) continue;

                            data.filteredItems.push_back(item.first);
                        }
                    }
                }
                break;
            case ModSpecial_All:
                auto dh = RE::TESDataHandler::GetSingleton();
                AddFormsToList(dh->GetFormArray<RE::TESObjectARMO>(), filter);
                AddFormsToList(dh->GetFormArray<RE::TESObjectWEAP>(), filter);
                AddFormsToList(dh->GetFormArray<RE::TESAmmo>(), filter);
                break;
        }
    }

    results.Clear();
    data.dvSets.clear();

    if (!data.filteredItems.empty()) {
        std::sort(data.filteredItems.begin(), data.filteredItems.end(),
                  [](RE::TESBoundObject* const a, RE::TESBoundObject* const b) { return _stricmp(a->GetName(), b->GetName()) < 0; });

        if (curMod) AnalyzeArmor(data.filteredItems, results);
    }

    return true;
}

bool WillBeModified(const ArmorChangeParams& params, RE::TESBoundObject* i, ArmorSlots remapped) {
    if (params.armorSet) {
        if (auto armor = i->As<RE::TESObjectARMO>()) {
            if (((remapped | g_Config.slotsWillChange) & (ArmorSlots)armor->GetSlotMask()) == 0) return false;
        } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
            if (!params.armorSet->FindMatching(weap)) return false;
        } else if (auto ammo = i->As<RE::TESAmmo>()) {
            if (!params.armorSet->FindMatching(ammo)) return false;
        } else
            return false;
        return true;
    } else {
        return g_Data.modifiedItems.contains(i);
    }
}

struct HighlightTrack {
    short round = -1;
    char pop = 0;
    bool enabled = false;

    void Touch() { round = g_filterRound; }
    operator bool() { return !enabled || g_filterRound == round; }

    void Push(bool show = true) {
        enabled = show;
        if (g_Config.bHighlights && round != g_filterRound && show) {
            auto phase = 0.5 + 0.5 * sin(ImGui::GetTime() * (std::_Pi_val / 1.0));
            auto brightness = std::lerp(64, 255, phase);
            const auto colorHighlight = IM_COL32(0, brightness, brightness, 255);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, colorHighlight);
            pop = 1;
        } else
            pop = 0;
    }

    void Pop() {
        if (ImGui::IsItemActive()) round = g_filterRound;

        ImGui::PopStyleVar(pop);
        ImGui::PopStyleColor(pop);
    }
};

struct TimedTooltip {
    std::string text;
    double until = 0;

    void Enable(std::string s) {
        text = s;
        until = ImGui::GetTime() + 10.0;
    }

    bool Show() {
        if (ImGui::GetTime() >= until) return false;

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(text.c_str());
        }
        return true;
    }
};

void BuildFonts() {
    auto& io = ImGui::GetIO();
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());

    auto AddNameGlyphs = [&](auto& ls) {
        for (auto i : ls) {
            builder.AddText(i->fullName.c_str());

            // Need to get lowercase versions of glyphs too
            std::string name(i->fullName.c_str());
            builder.AddText(toLowerUTF8(name).c_str());
        }
    };

    // Item Glyphs
    auto dh = RE::TESDataHandler::GetSingleton();
    AddNameGlyphs(dh->GetFormArray<RE::TESObjectARMO>());
    AddNameGlyphs(dh->GetFormArray<RE::TESObjectWEAP>());
    AddNameGlyphs(dh->GetFormArray<RE::TESAmmo>());

    // Translated text
    if (auto trans = Localization::Get()->mapTranslated) {
        for (auto& i : *trans) builder.AddText(i.second.c_str());
    }

    for (auto& i : g_Config.mapCustomKWTabs) builder.AddText(i.first.c_str());
    for (auto& i : g_Config.mapCustomKWs) {
        builder.AddText(i.second.name.c_str());
        builder.AddText(i.second.tooltip.c_str());
    }
    for (auto& i : g_Config.curves) {
        for (int s = 0; s < 32; s++) builder.AddText(i.second.slotName[s].c_str());
    }
    for (auto& i : g_Config.lootProfiles) builder.AddText(i.c_str());
    for (auto& i : g_Config.armorSets) builder.AddText(i.name.c_str());

    // Language glyphs - or else the switch language menu might be unusable
    struct LocaleEnum {
        static BOOL CALLBACK Proc(LPWSTR lpLocaleStr, DWORD, LPARAM lParam) {
            // Convert the wide-character locale name to a narrow string
            std::wstring ws(lpLocaleStr);

            size_t underscore_pos = ws.find(L'-');
            if (!underscore_pos) return TRUE;

            if (underscore_pos != std::string::npos) {
                ws = ws.substr(0, underscore_pos);
            }

            wchar_t languageName[256];
            if (GetLocaleInfoEx(ws.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, languageName, 256) > 0) {
                auto builder = reinterpret_cast<ImFontGlyphRangesBuilder*>(lParam);
                builder->AddText(WStringToString(languageName).c_str());
            }

            // Return true to continue enumeration
            return TRUE;
        }
    };

    EnumSystemLocalesEx(LocaleEnum::Proc, LOCALE_ALL, reinterpret_cast<LPARAM>(&builder), nullptr);

    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    io.Fonts->Clear();
    // io.Fonts->FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();
    //  io.Fonts->AddFontDefault();

    ImFontConfig config;

    const float fontSize = (float)g_Config.nFontSize;

    auto path = std::filesystem::current_path() / PATH_ROOT;
    path /= "fonts/";

    if (std::filesystem::exists(path)) {
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (!entry.is_regular_file()) continue;
                if (_stricmp(entry.path().extension().generic_string().c_str(), ".ttf")) continue;

                if (!io.Fonts->Fonts.empty()) config.MergeMode = true;

                io.Fonts->AddFontFromFileTTF(entry.path().generic_string().c_str(), fontSize, &config, ranges.Data);
            }
        }
    }

    if (io.Fonts->Fonts.empty()) {
        config.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&config);
    }

    // Need to force it to rebuild the font textures - suppose to be automatic, but isn't?

    if (io.Fonts->Build()) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
}

void QuickArmorRebalance::RenderUI() {
    const auto colorChanged = IM_COL32(0, 255, 0, 255);
    const auto colorChangedPartial = IM_COL32(0, 150, 0, 255);
    const auto colorChangedShared = IM_COL32(255, 255, 0, 255);
    const auto colorDeleted = IM_COL32(255, 0, 0, 255);

    const char* rarity[] = {LZ("Common"), LZ("Uncommon"), LZ("Rare"), nullptr};
    const char* itemTypes[] = {LZ("Any"), LZ("Armor"), LZ("Weapons"), LZ("Ammo"), nullptr};

    bool isActive = true;
    static std::set<RE::TESBoundObject*> selectedItems;
    static RE::TESBoundObject* lastSelectedItem = nullptr;
    static GivenItems givenItems;
    static ModData* curMod = nullptr;
    static std::set<RE::TESObject*> uncheckedItems;

    if (!RE::UI::GetSingleton()->numPausesGame) givenItems.recentEquipSlots = 0;

    const bool isShiftDown = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    const bool isCtrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool isAltDown = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);

    static ArmorChangeParams paramsScratch(g_Config.acData);
    static bool bUseScratchParams = false;

    ArmorChangeParams& params = bUseScratchParams ? paramsScratch : g_Config.acParams;
    auto& data = *params.data;
    static auto& analyzeResults = g_Config.acData.analyzeResults;

    static ItemFilter filter;

    static HighlightTrack hlConvert;
    static HighlightTrack hlDistributeAs;
    static HighlightTrack hlRarity;
    static HighlightTrack hlDynamicVariants;
    static HighlightTrack hlSlots;

    auto isInventoryOpen = RE::UI::GetSingleton()->IsItemMenuOpen();

    static bool bRebuildFonts = true;

    static bool bMenuHovered = false;
    static bool bSlotWarning = false;
    bool popupSettings = false;
    bool popupRemapSlots = false;
    bool popupDynamicVariants = false;
    bool popupCustomKeywords = false;

    bool hasModifiedItems = false;

    ArmorSlots remappedSrc = 0;
    ArmorSlots remappedTar = 0;

    ModData* switchToMod = nullptr;

    static int nShowWords = AnalyzeResults::eWords_EitherVariants;

    struct Local {
        static void SwitchToMod(ModData* mod) {
            curMod = mod;
            givenItems.items.clear();
            selectedItems.clear();
            lastSelectedItem = nullptr;

            g_Config.acParams.Reset();
            paramsScratch.Reset();

            g_filterRound++;
        }
    };

    for (auto i : params.mapArmorSlots) {
        remappedSrc |= 1 << i.first;
        remappedTar |= i.second < 32 ? (1 << i.second) : 0;
    }

    if (bRebuildFonts) {
        bRebuildFonts = false;
        ImGuiIntegration::LoadFont(BuildFonts);
    }

    std::string strSlotDesc[33];
    if (params.curve) {
        for (int i = 0; i < 32; i++) strSlotDesc[i] = LZFormat("Slot {} - {}", i + 30, LZ(params.curve->slotName[i].c_str()));
        strSlotDesc[32] = LZ("<REMOVE SLOT>");
    }

    ImGuiWindowFlags wndFlags = ImGuiWindowFlags_NoScrollbar;
    if (bMenuHovered) wndFlags |= ImGuiWindowFlags_MenuBar;

    ImGui::SetNextWindowSizeConstraints({700, 250}, {1600, 1000});
    if (ImGui::Begin("Quick Armor Rebalance", &isActive, wndFlags)) {
        if (g_Config.strCriticalError.empty()) {
            static int nModFilter = 0;
            const char* modFilterDesc[] = {LZ("No filter"), LZ("Unmodified"), LZ("Modified"), LZ("Has possible dynamic variants")};

            const char* strModSpecial[] = {LZ("<Currently Worn Armor>"), LZ("<All Items>"), nullptr};
            bool bModSpecialEnabled[] = {true, g_Config.bEnableAllItems};
            static int nModSpecial = 0;

            static bool bFilterChangedMods = false;

            if (bMenuHovered) {
                bMenuHovered = false;

                if (ImGui::BeginMenuBar()) {
                    auto menuSize = ImGui::GetItemRectSize();

                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::MenuItem(RightAlign(LZ("Settings")))) {
                        popupSettings = true;
                    }

                    ImGui::EndMenuBar();

                    // The menu bar doesn't return the right values (but somehow has the right size?), so instead we
                    // work off the last menu item added
                    bMenuHovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);  // Doesn't actually work
                    bMenuHovered |= ImGui::IsMouseHoveringRect(ImGui::GetWindowPos(), ImGui::GetItemRectMax(), false);

                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - menuSize.y);
                }
            }
            bMenuHovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);

            // ImGui::PushItemWidth(-FLT_MIN);
            ImGui::SetNextItemWidth(-FLT_MIN);

            if (ImGui::BeginTable("WindowTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("LeftCol", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("RightCol", ImGuiTableColumnFlags_WidthFixed, 0.25f * ImGui::GetContentRegionAvail().x);

                bool showPopup = false;

                ImGui::TableNextColumn();
                if (ImGui::BeginChild("LeftPane")) {
                    // ImGui::PushItemWidth(-FLT_MIN);

                    // ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginTable("Mod Table", 3, ImGuiTableFlags_SizingFixedFit, {-FLT_MIN, 0.0f})) {
                        ImGui::TableSetupColumn("ModCol1");
                        ImGui::TableSetupColumn("ModCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ModCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text(LZ("Mod"));
                        ImGui::TableNextColumn();

                        RE::TESFile* blacklist = nullptr;

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::BeginCombo("##Mod", curMod ? curMod->mod->fileName : strModSpecial[nModSpecial], ImGuiComboFlags_HeightLarge)) {
                            if (!nModFilter) {
                                for (int i = 0; strModSpecial[i]; i++) {
                                    if (!bModSpecialEnabled[i]) continue;
                                    bool selected = !curMod && i == nModSpecial;
                                    if (ImGui::Selectable(strModSpecial[i], selected)) {
                                        curMod = nullptr;
                                        nModSpecial = i;
                                        g_filterRound++;
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                            }

                            for (auto i : GetFilteredMods(nModFilter)) {
                                bool selected = curMod == i;

                                int pop = 0;
                                if (g_Data.modifiedFiles.contains(i->mod)) {
                                    // if (bFilterChangedMods) continue;
                                    if (g_Data.modifiedFilesDeleted.contains(i->mod))
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                    else
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                    pop++;
                                } else if (g_Data.modifiedFilesShared.contains(i->mod)) {
                                    // if (bFilterChangedMods) continue;
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                                    pop++;
                                }

                                if (ImGui::Selectable(i->mod->fileName, selected)) {
                                    if (isCtrlDown && isAltDown && g_Data.modifiedFiles.contains(i->mod)) {
                                        showPopup = true;
                                    }

                                    Local::SwitchToMod(i);
                                }
                                if (isCtrlDown && isAltDown && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                                    blacklist = i->mod;
                                }

                                if (selected) ImGui::SetItemDefaultFocus();
                                ImGui::PopStyleColor(pop);
                            }

                            ImGui::EndCombo();

                            data.isWornArmor = !curMod;

                            if (blacklist) g_Config.AddUserBlacklist(blacklist);
                        }

                        {
                            const char* popupTitle = LZ("Delete Changes?");
                            if (showPopup) ImGui::OpenPopup(popupTitle);

                            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

                            if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text(LZ("Delete all changes to %s?"), curMod->mod->fileName);
                                ImGui::Text(LZ("Changes will not revert until after restarting Skyrim"));

                                if (ImGui::Button(LZ("Delete changes"), ImVec2(120, 0))) {
                                    DeleteAllChanges(curMod->mod);
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SetItemDefaultFocus();
                                ImGui::SameLine();
                                if (ImGui::Button(LZ("Cancel"), ImVec2(120, 0))) {
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }

                        ImGui::TableNextColumn();

                        // ImGui::Checkbox("Hide modified", &bFilterChangedMods);

                        ImGui::SetNextItemWidth(200.0f);
                        ImGui::Combo("##FilterMods", &nModFilter, modFilterDesc, IM_ARRAYSIZE(modFilterDesc));
                        ImGui::EndTable();
                    }

                    ImGui::Separator();

                    ImGui::Text(LZ("Filter"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::InputTextWithHint("##ItemFilter", LZ("by Name"), filter.nameFilter, sizeof(filter.nameFilter) - 1, ImGuiInputTextFlags_AutoSelectAll))
                        g_filterRound++;

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);

                    int popCol = 0;
                    if (!filter.nType) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        popCol++;
                    }

                    if (ImGui::BeginCombo("##Typefilter", filter.nType ? itemTypes[filter.nType] : LZ("by Type"))) {
                        ImGui::PopStyleColor(popCol);
                        popCol = 0;

                        for (int i = 0; itemTypes[i]; i++) {
                            if (ImGui::Selectable(itemTypes[i])) {
                                filter.nType = i;
                                g_filterRound++;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopStyleColor(popCol);
                    popCol = 0;

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120);

                    popCol = 0;
                    if (!filter.slots) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        popCol++;
                    }

                    if (ImGui::BeginCombo("##Slotfilter", filter.slots ? LZ("Filtering by Slot") : LZ("by Slot"), ImGuiComboFlags_HeightLargest)) {
                        ImGui::PopStyleColor(popCol);
                        popCol = 0;

                        if (ImGui::Selectable(LZ("Clear filter"))) {
                            filter.slots = 0;
                            g_filterRound++;
                        }
                        if (ImGui::RadioButton(LZ("Any of"), &filter.slotMode, ItemFilter::SlotFilterMode::SlotsAny)) g_filterRound++;
                        if (ImGui::RadioButton(LZ("All of"), &filter.slotMode, ItemFilter::SlotFilterMode::SlotsAll)) g_filterRound++;
                        if (ImGui::RadioButton(LZ("Not"), &filter.slotMode, ItemFilter::SlotFilterMode::SlotsNot)) g_filterRound++;

                        constexpr auto slotCols = 4;
                        if (ImGui::BeginTable("##SlotFilterTable", slotCols)) {
                            for (int i = 0; i < 32 / slotCols; i++) {
                                for (int j = 0; j < slotCols; j++) {
                                    auto s = i + j * 32 / slotCols;
                                    bool bCheck = filter.slots & (1 << s);
                                    ImGui::TableNextColumn();
                                    if (ImGui::Checkbox(strSlotDesc[s].c_str(), &bCheck)) {
                                        g_filterRound++;
                                        if (bCheck)
                                            filter.slots |= (1 << s);
                                        else
                                            filter.slots &= ~(1 << s);
                                    }
                                }
                            }
                            ImGui::EndTable();
                        }

                        ImGui::EndCombo();
                    }
                    ImGui::PopStyleColor(popCol);
                    popCol = 0;

                    ImGui::SameLine();
                    if (ImGui::Checkbox(LZ("Unmodified"), &filter.bUnmodified)) g_filterRound++;

                    if (ImGui::BeginTable("Convert Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("ConvertCol1");
                        ImGui::TableSetupColumn("ConvertCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ConvertCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text(LZ("Convert to"));
                        ImGui::TableNextColumn();

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

                        int nArmorSet = 0;
                        hlConvert.Push();

                        const auto strSpecialConvert = LZ("<Keep previous>");

                        if (ImGui::BeginCombo("##ConvertTo", params.armorSet ? params.armorSet->name.c_str() : strSpecialConvert,
                                              ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                            {
                                bool selected = !params.armorSet;
                                if (ImGui::Selectable(strSpecialConvert, selected)) params.armorSet = nullptr;
                                if (selected) ImGui::SetItemDefaultFocus();
                                MakeTooltip(LZ("Keeps the existing conversion set.\nIf there is no existing conversion, no changes will be made to the item."), true);
                            }

                            for (auto& i : g_Config.armorSets) {
                                bool selected = params.armorSet == &i;
                                ImGui::PushID(nArmorSet++);
                                if (ImGui::Selectable(LZ(i.name.c_str()), selected)) params.armorSet = &i;
                                if (selected) ImGui::SetItemDefaultFocus();
                                MakeTooltip(i.strContents.c_str(), true);
                                ImGui::PopID();
                            }

                            ImGui::EndCombo();
                        }
                        hlConvert.Pop();

                        ImGui::TableNextColumn();

                        ImGui::Checkbox(LZ("Merge"), &params.bMerge);
                        MakeTooltip(
                            LZ("When enabled, merge will keep any previous changes not currently being modified.\n"
                               "Enable only the fields you want to be changing.\n"
                               "Disable this to clear all previous changes."));
                        ImGui::SameLine();

                        static HighlightTrack hlApply;
                        hlApply.Push(hlConvert && hlDistributeAs && hlRarity && hlSlots);

                        ImGui::BeginDisabled(data.filteredItems.size() >= kItemListLimit);

                        static TimedTooltip respApply;
                        bool bApply = false;
                        if (ImGui::Button(LZ("Apply changes"))) {
                            g_Config.Save();

                            if (data.items.size() < kItemApplyWarningThreshhold) {
                                bApply = true;
                            } else
                                ImGui::OpenPopup("###ApplyWarn");
                        }
                        respApply.Show();
                        ImGui::EndDisabled();
                        hlApply.Pop();

                        if (ImGui::BeginPopupModal("Warning###ApplyWarn", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text(LZ("This will change %d items, are you sure?"), data.items.size());

                            if (ImGui::Button(LZ("Yes, apply changes"))) {
                                bApply = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SetItemDefaultFocus();
                            ImGui::SameLine();
                            if (ImGui::Button(LZ("Cancel"))) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::EndTable();

                        if (bApply) {
                            auto r = MakeArmorChanges(params);
                            respApply.Enable(LZFormat("{} changes made", r));

                            if (g_Config.bAutoDeleteGiven) givenItems.Remove();

                            hlConvert.Touch();
                            hlDistributeAs.Touch();
                            hlRarity.Touch();
                            hlSlots.Touch();
                            hlDynamicVariants.Touch();
                        }
                    }

                    if (GetCurrentListItems(curMod, nModSpecial, filter, analyzeResults)) {
                        for (auto w : g_Config.wordsAutoDisable) {
                            const auto& it = analyzeResults.mapWordItems.find(w);
                            if (it != analyzeResults.mapWordItems.end()) {
                                for (auto item : it->second.items) uncheckedItems.insert(item);
                            }
                        }
                    }
                    g_Config.slotsWillChange = GetConvertableArmorSlots(params);

                    bool hasEnabledArmor = false;
                    bool hasEnabledWeap = false;

                    if (data.filteredItems.size() < kItemListLimit) {
                        for (auto i : data.filteredItems) {
                            if (i->As<RE::TESObjectARMO>()) {
                                if (!uncheckedItems.contains(i)) hasEnabledArmor = true;
                            } else if (i->As<RE::TESObjectWEAP>()) {
                                if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
                            } else if (i->As<RE::TESAmmo>()) {
                                if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
                            }
                        }
                    }

                    // Distribution
                    ImGui::Separator();
                    ImGui::BeginDisabled(!curMod || !params.armorSet);

                    // Need to create a dummy table to negate stretching the combo boxes
                    ImGui::Checkbox(LZ("Distribute as "), &params.bDistribute);
                    if (!curMod)
                        MakeTooltip(LZ("Distribution can only be configured for a single mod at a time."));
                    else if (!params.armorSet)
                        MakeTooltip(LZ("Cannot add or change distribution without a conversion set selected."));
                    else
                        MakeTooltip(LZ("Additions or changes to loot distribution will not take effect until you restart Skyrim"));
                    ImGui::BeginDisabled(!params.bDistribute);

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(300);

                    hlDistributeAs.Push(params.bDistribute);

                    if (ImGui::BeginCombo("##DistributeAs", params.distProfile, ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                        for (auto& i : g_Config.lootProfiles) {
                            bool selected = params.distProfile == i;
                            if (ImGui::Selectable(LZ(i.c_str()), selected)) params.distProfile = i.c_str();
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    hlDistributeAs.Pop();

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);

                    hlRarity.Push(params.bDistribute);

                    if (ImGui::BeginCombo("##Rarity", rarity[params.rarity], ImGuiComboFlags_PopupAlignLeft)) {
                        for (int i = 0; rarity[i]; i++) {
                            bool selected = i == params.rarity;
                            if (ImGui::Selectable(rarity[i], selected)) params.rarity = i;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    hlRarity.Pop();

                    ImGui::Indent(60);
                    ImGui::Checkbox(LZ("As pieces"), &params.bDistAsPieces);
                    ImGui::SameLine();
                    ImGui::Checkbox(LZ("As whole set"), &params.bDistAsSet);
                    MakeTooltip(LZ("You will find entire sets (all slots) together"));
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!params.bDistAsSet);
                    ImGui::Checkbox(LZ("Matching sets"), &params.bMatchSetPieces);
                    MakeTooltip(
                        LZ("Attempts to match sets together - for example, if there are green and blue variants, it will\n"
                           "try to distribute only green or only blue parts as a single set"));
                    ImGui::EndDisabled();

                    hlDynamicVariants.Push(!analyzeResults.sets[AnalyzeResults::eWords_DynamicVariants].empty() ||
                                           !analyzeResults.sets[AnalyzeResults::eWords_EitherVariants].empty());

                    ImGui::SameLine();
                    if (ImGui::Button(LZ("Dynamic Variants"))) {
                        popupDynamicVariants = true;
                    }
                    hlDynamicVariants.Pop();

                    ImGui::Unindent(60);

                    ImGui::EndDisabled();
                    ImGui::EndDisabled();

                    // Modifications
                    const float tabBoxHeight = 5 * (ImGui::GetFontSize() + 12);
                    if (ImGui::BeginChild("ItemTypeFrame", {ImGui::GetContentRegionAvail().x, tabBoxHeight}, false)) {
                        {
                            auto cursorPos = ImGui::GetCursorPos();
                            ImGui::SetNextItemAllowOverlap();
                            if (ImGui::Checkbox(RightAlign(LZ("Alt Settings"), ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x), &bUseScratchParams)) {
                                paramsScratch.Reset(true);
                                paramsScratch.Clear();
                            }
                            MakeTooltip(
                                LZ("Switches to settings with everything disabled.\n"
                                   "Useful for making individual changes when used with Merge enabled."));

                            ImGui::SetCursorPos(cursorPos);
                        }

                        if (ImGui::BeginTabBar("ItemTypeBar")) {
                            static int iTabOpen = 0;
                            int iTab = 0;
                            int iTabSelected = iTabOpen;

                            bool bTabEnabled[] = {hasEnabledArmor && (!params.armorSet || !params.armorSet->items.empty()),
                                                  hasEnabledWeap && (!params.armorSet || !params.armorSet->weaps.empty())};
                            const int nTabCount = 2;

                            bool bForceSelect = false;
                            if (!bTabEnabled[iTabOpen]) {
                                iTabOpen = 0;
                                while (iTabOpen < nTabCount && !bTabEnabled[iTabOpen]) iTabOpen++;
                                if (iTabOpen >= nTabCount) iTabOpen = 0;

                                bForceSelect = true;
                            }

                            const char* tabLabels[] = {LZ("Armor"), LZ("Weapons")};
                            // if (iTabOpen != iTabSelected) ImGui::SetTabItemClosed(tabLabels[iTabSelected]);

                            iTabSelected = iTabOpen;

                            // Amor tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(tabLabels[iTab], nullptr, bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow(LZ("Armor Rating"), params.armor.rating);
                                    SliderRow(LZ("Weight"), params.armor.weight);

                                    // Warmth
                                    {
                                        const char* warmthDesc[] = {
                                            LZ("None"),       //<0.1
                                            LZ("Cold"),       //<0.2
                                            LZ("Poor"),       //<0.3
                                            LZ("Limited"),    //<0.4
                                            LZ("Fair"),       //<0.5
                                            LZ("Average"),    //<0.6
                                            LZ("Good"),       //<0.7
                                            LZ("Warm"),       //<0.8
                                            LZ("Full"),       //<0.9
                                            LZ("Excellent"),  //<1.0
                                            LZ("Maximum"),    //=1.0
                                        };

                                        const char* coverageDesc[] = {
                                            LZ("None"),         //<0.1
                                            LZ("Minimal"),      //<0.2
                                            LZ("Poor"),         //<0.3
                                            LZ("Limited"),      //<0.4
                                            LZ("Fair"),         //<0.5
                                            LZ("Average"),      //<0.6
                                            LZ("Good"),         //<0.7
                                            LZ("Significant"),  //<0.8
                                            LZ("Full"),         //<0.9
                                            LZ("Excellent"),    //<1.0
                                            LZ("Maximum"),      //=1.0
                                        };

                                        bool showCoverage = g_Config.isFrostfallInstalled || g_Config.bShowFrostfallCoverage;

                                        auto& pair = params.armor.warmth;
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();

                                        ImGui::Checkbox(LZ("Modify Warmth"), &pair.bModify);
                                        MakeTooltip(
                                            LZ("The total warmth of the armor set.\n"
                                               "This number is NOT relative to the base armor set's warmth."));
                                        ImGui::BeginDisabled(!pair.bModify);
                                        ImGui::TableNextColumn();

                                        ImGui::SetNextItemWidth((showCoverage ? 0.5f : 1.0f) * ImGui::GetContentRegionAvail().x);
                                        ImGui::SliderFloat("##WarmthSlider", &pair.fScale, 0.f, 100.f, "%.0f%% Warmth", ImGuiSliderFlags_AlwaysClamp);
                                        MakeTooltip(warmthDesc[(int)(0.1f * pair.fScale)]);

                                        if (showCoverage) {
                                            ImGui::SameLine();
                                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                                            ImGui::SliderFloat("##CoverageSlider", &params.armor.coverage, 0.f, 100.f, "%.0f%% Coverage", ImGuiSliderFlags_AlwaysClamp);
                                            MakeTooltip(coverageDesc[(int)(0.1f * params.armor.coverage)]);
                                        }

                                        ImGui::TableNextColumn();
                                        if (ImGui::Button(LZ("Reset"))) {
                                            pair.fScale = 50.0f;
                                            params.armor.coverage = 50.0f;
                                        }
                                        ImGui::EndDisabled();
                                    }

                                    ImGui::EndTable();
                                }

                                ImGui::Separator();
                                ImGui::Text(LZ("Stat distribution curve"));
                                ImGui::SameLine();

                                static auto* curCurve = &g_Config.curves[0];

                                ImGui::SetNextItemWidth(220);
                                if (ImGui::BeginCombo("##Curve", curCurve->first.c_str(), ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                                    for (auto& i : g_Config.curves) {
                                        bool selected = curCurve == &i;
                                        ImGui::PushID(i.first.c_str());
                                        if (ImGui::Selectable(i.first.c_str(), selected)) curCurve = &i;
                                        if (selected) ImGui::SetItemDefaultFocus();
                                        ImGui::PopID();
                                    }

                                    ImGui::EndCombo();
                                }
                                params.curve = &curCurve->second;

                                ImGui::SameLine();
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40);

                                hlSlots.Push(bSlotWarning);
                                if (ImGui::Button(LZ("Remap Slots"))) popupRemapSlots = true;
                                hlSlots.Pop();

                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            // Weapon tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(tabLabels[iTab], nullptr, bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow(LZ("Damage"), params.weapon.damage);
                                    SliderRow(LZ("Weight"), params.weapon.weight);
                                    SliderRow(LZ("Speed"), params.weapon.speed);
                                    SliderRow(LZ("Stagger"), params.weapon.stagger);

                                    ImGui::EndTable();
                                }
                                iTab++;

                                ImGui::EndTabItem();
                            }
                            ImGui::EndDisabled();
                            ImGui::EndTabBar();
                            iTabOpen = iTabSelected;
                        }
                    }
                    ImGui::EndChild();

                    if (SliderTable()) {
                        SliderRow(LZ("Gold Value"), params.value);

                        ImGui::EndTable();
                    }

                    ImGui::Checkbox(LZ("Modify Keywords"), &params.bModifyKeywords);
                    MakeTooltip(LZ("Changes basic keywords to match those on the base item."));

                    ImGui::SameLine();
                    if (ImGui::Button(RightAlign(LZ("Custom Keywords")))) {
                        popupCustomKeywords = true;
                    }
                    MakeTooltip(
                        LZ("Allows you to add or remove any keywords.\n"
                           "Note: Adding and removing basic Skyrim keywords is already included \n"
                           "as part of armor conversion."));

                    if (ImGui::BeginTable("Crafting Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("CraftCol1");
                        ImGui::TableSetupColumn("CraftCol2");
                        ImGui::TableSetupColumn("CraftCol3");

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(LZ("Modify Temper Recipe"), &params.temper.bModify);
                        ImGui::BeginDisabled(!params.temper.bModify);
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(LZ("Create recipe if missing##Temper"), &params.temper.bNew);
                        MakeTooltip(
                            LZ("If an item being modified lacks an existing temper recipe to modify, checking\n"
                               "this will create one automatically"));
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(g_Config.fTemperGoldCostRatio > 0 ? LZ("Cost gold if no recipe to copy##Temper") : LZ("Make free if no recipe to copy##Temper"),
                                        &params.temper.bFree);
                        MakeTooltip(
                            LZ("If the item being copied lacks a tempering recipe, checking will remove all\n"
                               "components and conditions to temper the modified item"));
                        ImGui::EndDisabled();

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(LZ("Modify Crafting Recipe"), &params.craft.bModify);
                        ImGui::BeginDisabled(!params.craft.bModify);
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(LZ("Create recipe if missing##Craft"), &params.craft.bNew);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                LZ("If an item being modified lacks an existing crafting recipe to modify,\n"
                                   "checking this will create one automatically"));
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(g_Config.fCraftGoldCostRatio > 0 ? LZ("Cost gold if no recipe to copy##Craft") : LZ("Make free if no recipe to copy##Craft"),
                                        &params.craft.bFree);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                LZ("If the item being copied lacks a crafting recipe, checking will remove all\n"
                                   "components and conditions to craft the modified item"));
                        ImGui::EndDisabled();

                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();

                ImGui::TableNextColumn();
                if (data.filteredItems.size() >= kItemListLimit) {
                    ImGui::Text(
                        LZ("Too many items.\n"
                           "Limit the amount of items by using the filters."));
                } else {
                    ImGui::Text(LZFormat("{} Items", data.filteredItems.size()).c_str());

                    auto avail = ImGui::GetContentRegionAvail();
                    avail.y -= ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.y * 2;

                    auto player = RE::PlayerCharacter::GetSingleton();

                    data.items.clear();
                    data.items.reserve(data.filteredItems.size());

                    bool modChangesDeleted = curMod ? g_Data.modifiedFilesDeleted.contains(curMod->mod) : false;

                    ImGui::PushStyleColor(ImGuiCol_NavHighlight, IM_COL32(0, 255, 0, 255));

                    if (ImGui::BeginListBox("##Items", avail)) {
                        auto xPos = ImGui::GetCursorPosX();

                        if (ImGui::BeginPopupContextWindow()) {
                            if (ImGui::Selectable(LZ("Unequip worn items"))) {
                                givenItems.UnequipCurrent();
                            }

                            ImGui::Separator();

                            ImGui::BeginDisabled(selectedItems.empty());
                            if (ImGui::BeginMenu(LZ("Selected ..."))) {
                                if (ImGui::Selectable(LZ("Equip"))) {
                                    for (auto i : selectedItems) givenItems.Give(i, true, true);
                                }
                                if (ImGui::Selectable(LZ("Unequip"))) {
                                    for (auto i : selectedItems) givenItems.Unequip(i);
                                }

                                ImGui::Separator();

                                if (ImGui::Selectable(LZ("Enable"))) {
                                    for (auto i : selectedItems) uncheckedItems.erase(i);
                                }
                                if (ImGui::Selectable(LZ("Enable ONLY"))) {
                                    uncheckedItems.clear();
                                    uncheckedItems.insert(data.filteredItems.begin(), data.filteredItems.end());
                                    for (auto i : selectedItems) uncheckedItems.erase(i);
                                }
                                if (ImGui::Selectable(LZ("Disable"))) {
                                    for (auto i : selectedItems) uncheckedItems.insert(i);
                                }

                                ImGui::Separator();
                                ImGui::BeginDisabled(curMod);
                                if (ImGui::Selectable(LZ("Select source mod"))) {
                                    switchToMod = g_Data.modData[(*selectedItems.begin())->GetFile(0)].get();
                                }
                                ImGui::EndDisabled();

                                if (ImGui::BeginMenu(LZ("Delete changes ..."))) {
                                    if (MenuItemConfirmed(LZ("All"))) {
                                        DeleteChanges(selectedItems);
                                    }
                                    ImGui::Separator();
                                    if (MenuItemConfirmed(LZ("Loot Distribution"))) {
                                        const char* fields[] = {"loot", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }
                                    if (MenuItemConfirmed(LZ("Slots"))) {
                                        const char* fields[] = {"slots", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }
                                    if (MenuItemConfirmed(LZ("Crafting"))) {
                                        const char* fields[] = {"craft", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }
                                    if (MenuItemConfirmed(LZ("Tempering"))) {
                                        const char* fields[] = {"temper", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }

                                    ImGui::EndMenu();
                                }

                                ImGui::EndMenu();
                            }
                            ImGui::EndDisabled();

                            if (ImGui::BeginMenu(LZ("Select ..."))) {
                                if (ImGui::Selectable(LZ("Armor"))) {
                                    selectedItems.clear();
                                    for (auto i : data.filteredItems) {
                                        if (auto armor = i->As<RE::TESObjectARMO>()) selectedItems.insert(armor);
                                    }
                                }
                                if (ImGui::Selectable(LZ("Weapons"))) {
                                    selectedItems.clear();
                                    for (auto i : data.filteredItems) {
                                        if (auto weap = i->As<RE::TESObjectWEAP>()) selectedItems.insert(weap);
                                    }
                                }
                                if (ImGui::Selectable(LZ("Ammo"))) {
                                    selectedItems.clear();
                                    for (auto i : data.filteredItems) {
                                        if (auto ammo = i->As<RE::TESAmmo>()) selectedItems.insert(ammo);
                                    }
                                }

                                ImGui::EndMenu();
                            }

                            ImGui::Separator();
                            if (ImGui::Selectable(LZ("Enable all"))) uncheckedItems.clear();
                            if (ImGui::Selectable(LZ("Disable all")))
                                for (auto i : data.filteredItems) uncheckedItems.insert(i);

                            ImGui::EndPopup();
                        }

                        // Clear out selections that are no longer visible
                        auto lastSelected(std::move(selectedItems));
                        bool hasAnchor = false;
                        for (auto i : data.filteredItems) {
                            if (lastSelected.contains(i)) selectedItems.insert(i);
                            if (i == lastSelectedItem) hasAnchor = true;
                        }
                        if (!hasAnchor) lastSelectedItem = nullptr;

                        for (auto i : data.filteredItems) {
                            int popCol = 0;
                            bool isModified = false;
                            if (g_Data.modifiedItems.contains(i)) {
                                if (modChangesDeleted || g_Data.modifiedItemsDeleted.contains(i))
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                else {
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                    isModified = true;
                                }
                                popCol++;
                            } else if (g_Data.modifiedItemsShared.contains(i)) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                                popCol++;
                            } else if (!WillBeModified(params, i, remappedSrc)) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                popCol++;
                            }

                            std::string name(i->GetName());

                            if (name.empty()) name = std::format("{}:{:010x}", i->GetFile(0)->fileName, i->formID);

                            static RE::TESBoundObject* keyboardNav = nullptr;

                            ImGui::BeginGroup();

                            bool isChecked = !uncheckedItems.contains(i);
                            ImGui::PushID(name.c_str());

                            if (isChecked && isModified) hasModifiedItems = true;

                            if (ImGui::Checkbox("##ItemCheckbox", &isChecked)) {
                                if (!isChecked)
                                    uncheckedItems.insert(i);
                                else
                                    uncheckedItems.erase(i);
                            }
                            if (isChecked) data.items.push_back(i);

                            ImGui::SameLine();

                            int popVar = 0;
                            if (i == lastSelectedItem) {
                                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                                ImGui::PushStyleColor(ImGuiCol_FrameBg, colorChanged);

                                popVar++;
                                popCol++;
                            }

                            bool selected = selectedItems.contains(i);

                            if (i == keyboardNav) {
                                ImGui::SetKeyboardFocusHere();  // This will cause a frame of jitter if the name is too
                                                                // long, can't find a solution
                                keyboardNav = nullptr;
                            }

                            if (ImGui::Selectable(name.c_str(), &selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                                keyboardNav = nullptr;  // Force clear any pending keyboard navigation

                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                    if (isShiftDown) {
                                        if (isAltDown) {
                                            auto armorSet = BuildSetFrom(i, data.filteredItems);

                                            if (!isCtrlDown) selectedItems.clear();
                                            for (auto piece : armorSet) selectedItems.insert(piece);
                                        }
                                    } else {
                                        if (isAltDown) {
                                            auto armorSet = BuildSetFrom(i, data.filteredItems);
                                            givenItems.UnequipCurrent();
                                            for (auto piece : armorSet) givenItems.Give(piece, true);
                                        } else {
                                            givenItems.ToggleEquip(i);
                                        }
                                    }
                                } else {
                                    if (!isCtrlDown) {
                                        selectedItems.clear();
                                        selected = true;
                                    }

                                    if (!isShiftDown) {
                                        lastSelectedItem = i;
                                        if (selected)
                                            selectedItems.insert(i);
                                        else
                                            selectedItems.erase(i);
                                    } else {
                                        if (lastSelectedItem == i)
                                            selectedItems.insert(i);
                                        else if (lastSelectedItem) {
                                            bool adding = false;
                                            for (auto j : data.filteredItems) {
                                                if (j == i || j == lastSelectedItem) {
                                                    if (adding) {
                                                        selectedItems.insert(j);
                                                        break;
                                                    }
                                                    adding = true;
                                                }

                                                if (adding) selectedItems.insert(j);
                                            }
                                        }
                                    }
                                }
                            }

                            if (ImGui::IsItemFocused()) {
                                ImGui::SetScrollFromPosX(xPos - ImGui::GetCursorPosX(),
                                                         1.0f);  // don't know why this is 1.0 instead of 0, but it works

                                auto draw = ImGui::GetWindowDrawList();
                                draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), colorChanged);

                                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_Keypad8)) {
                                    auto it = std::find(data.filteredItems.begin(), data.filteredItems.end(), i);
                                    if (it != data.filteredItems.end() && it != data.filteredItems.begin()) {
                                        keyboardNav = *(it - 1);

                                        if (isShiftDown) {
                                            if (!isCtrlDown) {
                                                selectedItems.clear();
                                                selected = false;
                                            }

                                            if (lastSelectedItem == keyboardNav)
                                                selectedItems.insert(keyboardNav);
                                            else if (lastSelectedItem) {
                                                bool adding = false;
                                                for (auto j : data.filteredItems) {
                                                    if (j == keyboardNav || j == lastSelectedItem) {
                                                        if (adding) {
                                                            selectedItems.insert(j);
                                                            break;
                                                        }
                                                        adding = true;
                                                    }

                                                    if (adding) selectedItems.insert(j);
                                                }
                                            }
                                        } else if (!isCtrlDown) {
                                            selectedItems.clear();
                                            lastSelectedItem = keyboardNav;
                                            selectedItems.insert(keyboardNav);
                                        }
                                    }
                                } else if (!keyboardNav &&  // or else it scrolls to bottom
                                           (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_Keypad2))) {
                                    auto it = std::find(data.filteredItems.begin(), data.filteredItems.end(), i);
                                    if (it != data.filteredItems.end() && it != data.filteredItems.end() - 1) {
                                        keyboardNav = *(it + 1);

                                        if (isShiftDown) {
                                            if (!isCtrlDown) {
                                                selectedItems.clear();
                                                selected = false;
                                            }

                                            if (lastSelectedItem == keyboardNav)
                                                selectedItems.insert(keyboardNav);
                                            else if (lastSelectedItem) {
                                                bool adding = false;
                                                for (auto j : data.filteredItems) {
                                                    if (j == keyboardNav || j == lastSelectedItem) {
                                                        if (adding) {
                                                            selectedItems.insert(j);
                                                            break;
                                                        }
                                                        adding = true;
                                                    }

                                                    if (adding) selectedItems.insert(j);
                                                }
                                            }
                                        } else if (!isCtrlDown) {
                                            selectedItems.clear();
                                            lastSelectedItem = keyboardNav;
                                            selectedItems.insert(keyboardNav);
                                        }
                                    }
                                } else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                                    for (auto j : selectedItems) givenItems.ToggleEquip(j);
                                } else if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                                    if (isAltDown) {
                                        if (!selectedItems.empty()) {
                                            if (uncheckedItems.contains(*selectedItems.begin())) {
                                                for (auto j : selectedItems) uncheckedItems.erase(j);
                                            } else
                                                for (auto j : selectedItems) uncheckedItems.insert(j);
                                        }
                                    } else {
                                        lastSelectedItem = i;
                                        if (selectedItems.contains(i))
                                            selectedItems.erase(i);
                                        else
                                            selectedItems.insert(i);
                                    }
                                } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                                    for (auto j : selectedItems) givenItems.Remove(j);
                                } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                                    givenItems.Pop();
                                }
                            }

                            ImGui::PopID();
                            ImGui::EndGroup();

                            if (!curMod && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary)) {
                                if (ImGui::BeginTooltip()) {
                                    if (!curMod) ImGui::Text(LZ("File: %s"), i->GetFile(0)->fileName);
                                    ImGui::EndTooltip();
                                }
                            }

                            ImGui::PopStyleColor(popCol);
                            ImGui::PopStyleVar(popVar);
                        }

                        ImGui::EndListBox();
                    }

                    ImGui::PopStyleColor();

                    ImGui::BeginDisabled(!player || !curMod || isInventoryOpen);

                    if (ImGui::Button(selectedItems.empty() ? LZ("Give All") : LZ("Give Selected"))) {
                        if (selectedItems.empty())
                            for (auto i : data.filteredItems) {
                                givenItems.Give(i);
                            }
                        else
                            for (auto i : selectedItems) {
                                givenItems.Give(i);
                            }
                    }
                    if (isInventoryOpen) MakeTooltip(LZ("Can't use while inventory is open"));

                    ImGui::SameLine();
                    if (ImGui::Button(selectedItems.empty() ? LZ("Equip All") : LZ("Equip Selected"))) {
                        givenItems.UnequipCurrent();
                        if (selectedItems.empty())
                            for (auto i : data.filteredItems) {
                                givenItems.Give(i, true);
                            }
                        else
                            for (auto i : selectedItems) {
                                givenItems.Give(i, true);
                            }
                    }
                    if (isInventoryOpen) MakeTooltip(LZ("Can't use while inventory is open"));

                    ImGui::BeginDisabled(givenItems.items.empty());
                    ImGui::SameLine();
                    if (ImGui::Button(LZ("Delete Given"))) {
                        givenItems.Remove();
                    }
                    if (isInventoryOpen) MakeTooltip(LZ("Can't use while inventory is open"));

                    ImGui::EndDisabled();  // givenItems.empty()
                    ImGui::EndDisabled();  //! player

                    bSlotWarning = false;
                    for (auto i : data.items) {
                        if (auto armor = i->As<RE::TESObjectARMO>()) {
                            auto itemSlots = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
                            if ((~g_Config.usedSlotsMask) & (~remappedSrc) & itemSlots) {
                                bSlotWarning = true;
                                break;
                            }
                        }
                    }
                }

                ImGui::EndTable();
            }

            // ImGui::PopItemWidth();
        } else {
            ImGui::Text(g_Config.strCriticalError.c_str());
        }

        static char bufDisable[4096] = "";

        if (popupSettings) {
            ImGui::OpenPopup(LZ("Settings"));

            std::string strDisable;
            for (auto& w : g_Config.lsDisableWords) {
                if (!strDisable.empty()) strDisable += "\n";
                strDisable += w;
            }
            strDisable.copy(bufDisable, sizeof(bufDisable));
            bufDisable[sizeof(bufDisable) - 1] = '\0';
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        bool bPopupActive = true;
        if (ImGui::BeginPopupModal("Settings", &bPopupActive, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(LZ("NOTE: Many changes require restarting Skyrim to take full effect."));
            if (ImGui::BeginTabBar(LZ("Settings Tabs"))) {
                if (ImGui::BeginTabItem(LZ("General"))) {
                    ImGui::Text(LZ("Language"));
                    ImGui::SameLine();

                    std::string language;
                    {
                        wchar_t languageName[256];
                        if (GetLocaleInfoEx(Localization::Get()->language.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, languageName, 256) > 0) {
                            language = WStringToString(std::wstring(languageName));
                        } else {
                            language = "English";
                            Localization::Get()->SetTranslation(L"en");
                        }
                    }

                    if (ImGui::BeginCombo("##Language", language.c_str(), ImGuiComboFlags_PopupAlignLeft)) {
                        static std::vector<std::pair<std::wstring, std::string>> languagesTranslated;
                        static std::vector<std::pair<std::wstring, std::string>> languagesUntranslated;

                        if (languagesTranslated.empty() && languagesUntranslated.empty()) {
                            std::set<std::wstring> codes;
                            struct LocaleEnum {
                                static BOOL CALLBACK Proc(LPWSTR lpLocaleStr, DWORD, LPARAM lParam) {
                                    // Convert the wide-character locale name to a narrow string
                                    std::wstring ws(lpLocaleStr);

                                    size_t underscore_pos = ws.find(L'-');
                                    if (!underscore_pos) return TRUE;

                                    if (underscore_pos != std::string::npos) {
                                        ws = ws.substr(0, underscore_pos);
                                    }
                                    auto locales = reinterpret_cast<std::set<std::wstring>*>(lParam);
                                    locales->insert(ws);

                                    // Return true to continue enumeration
                                    return TRUE;
                                }
                            };

                            EnumSystemLocalesEx(LocaleEnum::Proc, LOCALE_ALL, reinterpret_cast<LPARAM>(&codes), nullptr);

                            languagesTranslated.reserve(Localization::Get()->translations.size());
                            languagesUntranslated.reserve(codes.size());

                            for (auto& i : codes) {
                                wchar_t languageName[256];

                                if (GetLocaleInfoEx(i.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, languageName, 256) > 0) {
                                    auto str = WStringToString(std::wstring(languageName));
                                    if (Localization::Get()->HasTranslation(i.c_str()))
                                        languagesTranslated.push_back({i, str});
                                    else
                                        languagesUntranslated.push_back({i, str});
                                }
                            }

                            std::sort(languagesTranslated.begin(), languagesTranslated.end(), [](auto& a, auto& b) { return a.second.compare(b.second) < 0; });
                            std::sort(languagesUntranslated.begin(), languagesUntranslated.end(), [](auto& a, auto& b) { return a.second.compare(b.second) < 0; });
                        }

                        auto Entry = [](auto& i) {
                            bool selected = !Localization::Get()->language.compare(i.first);
                            if (ImGui::Selectable(i.second.c_str(), selected)) {
                                Localization::Get()->SetTranslation(i.first);
                                bRebuildFonts = true;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        };
                        std::for_each(languagesTranslated.begin(), languagesTranslated.end(), Entry);

                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        std::for_each(languagesUntranslated.begin(), languagesUntranslated.end(), Entry);
                        ImGui::PopStyleColor();

                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    ImGui::Checkbox(LZ("Export untranslated strings"), &g_Config.bExportUntranslated);

                    if (ImGui::SliderInt(LZ("Font Size"), &g_Config.nFontSize, 10, 24, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                        bRebuildFonts = true;
                    }

                    const char* logLevels[] = {LZ("Trace"), LZ("Debug"), LZ("Info"), LZ("Warn"), LZ("Error"), LZ("Critical"), LZ("Off")};

                    ImGui::Text(LZ("Log verbsity"));
                    ImGui::SameLine();
                    if (ImGui::BeginCombo("##Verbosity", logLevels[g_Config.verbosity], ImGuiComboFlags_PopupAlignLeft)) {
                        for (int i = 0; i < spdlog::level::n_levels; i++) {
                            bool selected = g_Config.verbosity == i;
                            if (ImGui::Selectable(logLevels[i], selected)) {
                                g_Config.verbosity = i;
                                spdlog::set_level((spdlog::level::level_enum)i);
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::Checkbox(LZ("Close console after qar command"), &g_Config.bCloseConsole);
                    ImGui::Checkbox(LZ("Delete given items after applying changes"), &g_Config.bAutoDeleteGiven);
                    ImGui::Checkbox(LZ("Round weights to 0.1"), &g_Config.bRoundWeight);
                    ImGui::Checkbox(LZ("Reset sliders after changing mods"), &g_Config.bResetSliders);
                    ImGui::Checkbox(LZ("Reset slot remapping after changing mods"), &g_Config.bResetSlotRemap);
                    ImGui::Checkbox(LZ("Allow remapping armor slots to unhandled slots"), &g_Config.bAllowInvalidRemap);
                    ImGui::Checkbox(LZ("Allow remapping of protected armor slots (Not recommended)"), &g_Config.bEnableProtectedSlotRemapping);
                    MakeTooltip(
                        LZ("Body, hands, feet, and head slots tend to break in various ways if you move them to other "
                           "slots."));
                    ImGui::Checkbox(LZ("Highlight things you may want to look at"), &g_Config.bHighlights);
                    ImGui::Checkbox(LZ("Show Frostfall coverage slider even if not installed"), &g_Config.bShowFrostfallCoverage);
                    ImGui::Checkbox(LZ("USE AT YOUR OWN RISK: Enable <All Items> in item list"), &g_Config.bEnableAllItems);
                    MakeTooltip(
                        LZ("This can result in performance issues, and making a mess by changing too many items at once.\n"
                           "Use with caution."));

                    ImGui::Text(LZ("Automatically disable items with the following words (one per line):"));
                    if (ImGui::InputTextMultiline("##DisableWords", bufDisable, sizeof(bufDisable), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5))) {
                        std::vector<std::string> lines;
                        std::istringstream stream(bufDisable);
                        std::string line;

                        while (std::getline(stream, line)) {
                            // Trim whitespace
                            size_t start = line.find_first_not_of(" \t\n\r");
                            if (start == std::string::npos) continue;  // String is all whitespace
                            size_t end = line.find_last_not_of(" \t\n\r");
                            line = line.substr(start, end - start + 1);
                            lines.push_back(MakeLower(line));
                        }

                        g_Config.lsDisableWords = std::move(lines);
                        g_Config.RebuildDisabledWords();
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Distribution"))) {
                    ImGui::Checkbox(LZ("Enforce loot rarity with empty loot"), &g_Config.bEnableRarityNullLoot);
                    MakeTooltip(
                        LZ("Example: The subset of items elible for loot has 1 rare item, 0 commons and uncommons\n"
                           "DISABLED: You will always get that rare item\n"
                           "ENABLED: You will have a 5%% chance at the item, and 95%% chance of nothing"));

                    ImGui::Checkbox(LZ("Normalize drop rates between mods"), &g_Config.bNormalizeModDrops);
                    ImGui::SliderFloat(LZ("Adjust drop rates"), &g_Config.fDropRates, 0.0f, 300.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SliderInt(LZ("Drop curve level granularity"), &g_Config.levelGranularity, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(LZ("A lower number generates more loot lists but more accurate distribution"));

                    ImGui::Checkbox(LZ("Prevent distribution of dynamic variants"), &g_Config.bPreventDistributionOfDynamicVariants);

                    ImGui::SeparatorText(LZ("Preferred Variants"));
                    MakeTooltip(
                        LZ("When two items exist, one with the word and one without,\n"
                           "these settings will determine which will appear.\n\n"
                           "For example: Mod contains items 'Cape' and 'Cape (SMP)'\n"
                           "Setting SMP as preferred will cause only 'Cape (SMP)' to appear in loot."));

                    const char* prefDesc[] = {LZ("Don't care"), LZ("Prefer items with"), LZ("Prefer items without")};

                    ImGui::PushItemWidth(-FLT_MIN);
                    if (ImGui::BeginTable(LZ("Preference Variants"), 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn(LZ("Word"));
                        ImGui::TableSetupColumn(LZ("Options"));

                        for (auto& i : g_Config.mapPrefVariants) {
                            ImGui::TableNextColumn();
                            // ImGui::PushID(i.first.c_str());
                            ImGui::Text(i.first.c_str());

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(200.0f);
                            ImGui::Combo("##PrefCombo", &i.second.pref, prefDesc, IM_ARRAYSIZE(prefDesc));
                            // ImGui::PopID();
                        }

                        ImGui::EndTable();
                    }

                    static TimedTooltip resp;
                    if (ImGui::Button(LZ("Rescan ALL modified items for preference words"))) {
                        auto r = RescanPreferenceVariants();
                        resp.Enable(LZFormat("{} matching items found", r));
                    }
                    if (!resp.Show())
                        MakeTooltip(
                            LZ("You need only press this when new words are added to the list above.\n"
                               "You do not need to rescan when simply changing preferences."));

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Crafting"))) {
                    ImGui::Text(LZ("Only generate crafting recipes for"));
                    ImGui::SameLine();

                    if (ImGui::BeginCombo("##Rarity", rarity[g_Config.craftingRarityMax], ImGuiComboFlags_PopupAlignLeft)) {
                        for (int i = 0; rarity[i]; i++) {
                            bool selected = i == g_Config.craftingRarityMax;
                            if (ImGui::Selectable(rarity[i], selected)) g_Config.craftingRarityMax = i;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    ImGui::Text(LZ("rarity and below"));

                    ImGui::Indent();
                    ImGui::Checkbox(LZ("Disable existing crafting recipies above that rarity"), &g_Config.bDisableCraftingRecipesOnRarity);
                    ImGui::Unindent();

                    ImGui::Checkbox(LZ("Keep crafting books as requirement"), &g_Config.bKeepCraftingBooks);

                    ImGui::Checkbox(LZ("Use recipes from as similar item if primary item is lacking"), &g_Config.bUseSecondaryRecipes);

                    ImGui::Checkbox(LZ("Enable smelting recipes"), &g_Config.bEnableSmeltingRecipes);

                    ImGui::Text(LZ("Instead of free recipes, make recipes cost gold as a portion of the item's value:"));
                    ImGui::Indent();
                    ImGui::Text(LZ("Temper recipes"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##TemperRatio", &g_Config.fTemperGoldCostRatio, 0.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(LZ("Set to 0%% to keep free"));
                    ImGui::Text(LZ("Craft recipes"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##CraftRatio", &g_Config.fCraftGoldCostRatio, 0.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(LZ("Set to 0%% to keep free"));
                    ImGui::Unindent();

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Permissions"))) {
                    if (ImGui::BeginTable(LZ("Permissions"), 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableColumnFlags_NoReorder)) {
                        ImGui::TableSetupColumn(LZ("Local Changes Permissions"));
                        ImGui::TableSetupColumn(LZ("Shared Changes Permissions"));

                        ImGui::TableHeadersRow();
                        ImGui::TableNextRow();

                        ImGui::TableNextColumn();
                        PermissionsChecklist(LZ("Local"), g_Config.permLocal);

                        ImGui::TableNextColumn();
                        PermissionsChecklist(LZ("Shared"), g_Config.permShared);

                        ImGui::EndTable();
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Hooks"))) {
                    ImGui::Checkbox(LZ("Enable NIF armor slot remapping hook"), &g_Config.bEnableArmorSlotModelFixHook);
                    MakeTooltip(LZ("Fixes armor pieces not rendering when remapping their armor slots"));

                    ImGui::BeginDisabled(REL::Module::IsVR());
                    ImGui::Checkbox(LZ("Enable Skyrim warmth system hook"), &g_Config.bEnableSkyrimWarmthHook);
                    MakeTooltip(
                        LZ("Required to enable changes to item warmth.\n"
                           "WARNING: Likely to cause crashes on VR!"));
                    ImGui::EndDisabled();

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Integrations"))) {
                    ImGui::SeparatorText(LZ("Dynamic Armor Variants"));
                    ImGui::Checkbox(LZ("Enable exports to DAV"), &g_Config.bEnableDAVExports);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!g_Config.bEnableDAVExports);
                    ImGui::Checkbox(LZ("Even if DAV not present"), &g_Config.bEnableDAVExportsAlways);
                    if (ImGui::Button(LZ("Re-export all files to DAV"))) {
                        ExportAllToDAV();
                    }
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::EndPopup();
        }
        if (!bPopupActive) g_Config.Save();

        if (popupRemapSlots) ImGui::OpenPopup(LZ("Remap Slots"));

        ImGui::SetNextWindowSizeConstraints({300, 500}, {1600, 1000});
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;
        if (ImGui::BeginPopupModal(LZ("Remap Slots"), &bPopupActive, ImGuiWindowFlags_NoScrollbar)) {
            static int nSlotView = 33;
            uint64_t slotsUsed = 0;
            ArmorSlots slotsProtected = g_Config.bEnableProtectedSlotRemapping ? 0 : kProtectedSlotMask;

            std::vector<RE::TESObjectARMO*> lsSlotItems;
            for (auto i : data.items) {
                if (auto armor = i->As<RE::TESObjectARMO>()) {
                    auto itemSlots = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
                    slotsUsed |= itemSlots;
                    if (itemSlots & ((uint64_t)1 << nSlotView)) lsSlotItems.push_back(armor);
                }
            }

            nSlotView = 33;
            if (auto payload = ImGui::GetDragDropPayload()) {
                if (payload->IsDataType("ARMOR SLOT")) {
                    nSlotView = *(int*)payload->Data;
                }
            }

            ImGui::Text(LZ("Click and drag from the original slot on the left to the new replacement slot on the right"));
            ImGui::PushItemWidth(-FLT_MIN);

            if (ImGui::BeginTable(
                    "Slot Mapping", 3,
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_PreciseWidths | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn(LZ("Original"));
                ImGui::TableSetupColumn(LZ("Items"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(LZ("Remapped"));

                ImGui::TableHeadersRow();
                ImGui::TableNextRow();

                ImVec2 srcCenter[33];
                ImVec2 tarCenter[33];

                for (int i = 0; i < 33; i++) {
                    bool bProtected = i < 32 ? slotsProtected & (1 << i) : false;

                    ImGui::TableNextColumn();
                    if (i < 32) {
                        ImGui::BeginDisabled(bProtected || (((uint64_t)1 << i) & slotsUsed) == 0);
                        ImGui::BeginGroup();

                        const char* strWarn = nullptr;
                        int popCol = 0;
                        if ((((uint64_t)1 << i) & remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                            popCol++;
                        } else if ((((uint64_t)1 << i) & slotsUsed & ~(uint64_t)g_Config.usedSlotsMask)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            strWarn = LZ("Warning: Items in this slot will not be changed unless remapped to another slot.");
                            popCol++;
                        } else if ((1ull << i) & slotsUsed & (remappedTar & ~remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            strWarn =
                                LZ("Warning: Other items are being remapped to this slot.\n"
                                   "This will cause conflicts unless this slot is also remapped.");
                            popCol++;
                        }

                        bool bSelected = false;
                        if (ImGui::Selectable(strSlotDesc[i].c_str(), &bSelected, 0)) {
                        }

                        if (strWarn) MakeTooltip(strWarn);

                        if (ImGui::BeginDragDropSource(0)) {
                            nSlotView = i;
                            params.mapArmorSlots.erase(i);
                            ImGui::SetDragDropPayload("ARMOR SLOT", &i, sizeof(i));
                            ImGui::Text(strSlotDesc[i].c_str());
                            ImGui::EndDragDropSource();
                        }

                        ImGui::SameLine();

                        float w = ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.x * 2;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);  // Radio circles are weirdly offset down slightly?

                        ImGui::PushID("Source");
                        ImGui::PushID(i);
                        bool bCheckbox = false;
                        if (ImGui::RadioButton("##ItemCheckbox", &bCheckbox)) {
                        }
                        srcCenter[i] = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) / 2;
                        ImGui::PopID();
                        ImGui::PopID();
                        ImGui::PopStyleColor(popCol);

                        ImGui::EndGroup();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) nSlotView = i;

                        ImGui::EndDisabled();
                    }

                    ImGui::TableNextColumn();
                    if (i < lsSlotItems.size()) ImGui::Text(lsSlotItems[i]->GetName());

                    ImGui::TableNextColumn();
                    auto bDisabled = i != 32 && ((1 << i) & g_Config.usedSlotsMask) == 0;
                    ImGui::BeginDisabled(bProtected || bDisabled);
                    ImGui::BeginGroup();

                    bool bWarn = false;

                    int popCol = 0;
                    if ((((uint64_t)1 << i) & slotsUsed & (remappedTar & ~remappedSrc))) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                        bWarn = true;
                        popCol++;
                    } else if ((((uint64_t)1 << i) & remappedTar)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                        popCol++;
                    }

                    ImGui::PushID("Target");
                    ImGui::PushID(i);
                    bool bCheckbox = false;
                    bool bSelected = false;
                    if (ImGui::Selectable("##Select", &bSelected, 0)) {
                    }
                    if (bWarn)
                        MakeTooltip(
                            LZ("Warning: Items are being remapped to this slot, but other items are already using this "
                               "slot."));

                    if (!bProtected && (g_Config.bAllowInvalidRemap || !bDisabled) && ImGui::BeginDragDropTarget()) {
                        if (auto payload = ImGui::AcceptDragDropPayload("ARMOR SLOT")) {
                            params.mapArmorSlots[*(int*)payload->Data] = i;
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(strSlotDesc[i].c_str(), &bCheckbox)) {
                    }
                    tarCenter[i] = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) / 2;
                    tarCenter[i].x = ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().y - tarCenter[i].y;  // Want center of the circle
                    ImGui::PopID();
                    ImGui::PopID();

                    ImGui::PopStyleColor(popCol);
                    ImGui::EndGroup();

                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) nSlotView = i;
                }

                ImGui::EndTable();

                constexpr auto lineWidth = 3.0f;
                auto draw = ImGui::GetWindowDrawList();

                if (auto payload = ImGui::GetDragDropPayload()) {
                    if (payload->IsDataType("ARMOR SLOT")) {
                        draw->AddLine(srcCenter[*(int*)payload->Data], ImGui::GetMousePos(), colorChanged, lineWidth);
                    }
                }

                for (auto i : params.mapArmorSlots) {
                    draw->AddLine(srcCenter[i.first], tarCenter[i.second], colorChanged, lineWidth);
                }
            }

            ImGui::EndPopup();
        }

        static WordSet wordsUsed;
        static WordSet wordsStatic;
        static std::map<const DynamicVariant*, std::vector<std::size_t>> mapDVWords;

        static bool bDVChanged = false;

        if (popupDynamicVariants) {
            ImGui::OpenPopup(LZ("Dynamic Variants"));

            static short nDVFilterRound = -1;
            if (nDVFilterRound != g_filterRound) {
                nDVFilterRound = g_filterRound;

                nShowWords = AnalyzeResults::eWords_StaticVariants;
                wordsStatic.clear();
                wordsUsed.clear();
                mapDVWords.clear();

                for (auto& dv : g_Config.mapDynamicVariants) {
                    if (!dv.second.autos.empty()) {
                        for (auto w : dv.second.autos) {
                            if (analyzeResults.mapWordItems.find(w) != analyzeResults.mapWordItems.end()) {
                                mapDVWords[&dv.second].push_back(w);
                                wordsUsed.insert(w);
                            }
                        }
                    }
                }

                auto& ws = analyzeResults.sets[AnalyzeResults::eWords_StaticVariants];
                wordsStatic.insert(ws.begin(), ws.end());

                wordsUsed.insert(wordsStatic.begin(), wordsStatic.end());
                for (auto& i : mapDVWords) {
                    wordsUsed.insert(i.second.begin(), i.second.end());
                }

                bDVChanged = true;
            }
        }

        ImGui::SetNextWindowSizeConstraints({450, 300}, {1600, 1000});
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;

        static bool dvWndWasOpen = false;

        if (ImGui::BeginPopupModal(LZ("Dynamic Variants"), &bPopupActive, ImGuiWindowFlags_NoScrollbar)) {
            dvWndWasOpen = true;
            ImGui::Text(LZ("Drag the appropriate words (if any) to their associated dynamic type on the right side."));

            static TimedTooltip resp;
            ImGui::BeginDisabled(!hasModifiedItems);
            if (ImGui::Button(RightAlign(LZ("Update Dynamic Variants")))) {
                data.dvSets = MapVariants(analyzeResults, mapDVWords);
                auto r = AddDynamicVariants(params);

                resp.Enable(LZFormat("{} dynamic variants added", r));
            }

            if (!resp.Show()) {
                if (!hasModifiedItems)
                    MakeTooltip(
                        LZ("This only updates previously modified items, but none have been detected.\n\n"
                           "Dynamic variants will be included when clicking Apply Changes after closing this window."));
            }

            ImGui::EndDisabled();

            ImGui::SetNextItemWidth(-FLT_MIN);

            struct DragDropWords {
                static void Source(std::size_t w) {
                    if (ImGui::BeginDragDropSource(0)) {
                        std::vector<std::size_t> words;
                        words.push_back(w);

                        ImGui::SetDragDropPayload("WORD HASHES", &words[0], words.size() * sizeof(words[0]));
                        ImGui::Text(analyzeResults.mapWordStrings[w].c_str());
                        ImGui::EndDragDropSource();
                    }
                }

                static void TargetCommon(const std::function<void(std::size_t)> fnInsert) {
                    if (ImGui::BeginDragDropTarget()) {
                        if (auto payload = ImGui::AcceptDragDropPayload("WORD HASHES")) {
                            auto* pWords = (std::size_t*)payload->Data;
                            int nWords = payload->DataSize / sizeof(*pWords);

                            for (int i = 0; i < nWords; i++) {
                                auto w = *pWords++;
                                wordsStatic.erase(w);
                                for (auto& dv : mapDVWords) {
                                    auto it = std::find(dv.second.begin(), dv.second.end(), w);
                                    if (it != dv.second.end()) dv.second.erase(it);
                                }
                                wordsUsed.erase(w);

                                fnInsert(w);
                            }

                            bDVChanged = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                static void Target(WordSet* pSet) {
                    TargetCommon([&](std::size_t w) {
                        if (pSet) {
                            pSet->insert(w);
                            wordsUsed.insert(w);
                        }
                    });
                }

                static void Target(std::vector<std::size_t>* pSet, std::size_t at) {
                    TargetCommon([&](std::size_t w) {
                        if (pSet) {
                            pSet->insert(at ? std::find(pSet->begin(), pSet->end(), at) : pSet->begin(), w);
                            wordsUsed.insert(w);
                        }
                    });
                }
            };

            if (ImGui::BeginTable("WindowTable", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn(LZ("Static Variants"));
                ImGui::TableSetupColumn(LZ("Unassigned"));
                ImGui::TableSetupColumn(LZ("Dynamic Variants"));
                ImGui::TableSetupColumn(LZ("Output Variant Sets"), ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Static Variants"));
                MakeTooltip(
                    LZ("Static Variants are similar but distinct items.\n"
                       "Usually colors or different versions of an item piece.\n\n"
                       "The accurracy of words in this table is not important."));

                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Unassigned"));

                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Dynamic Variants"));
                MakeTooltip(
                    LZ("Dynamic Variants are the same item in different forms.\n\n"
                       "Place associated words under the associated variant type.\n"
                       "If there are multiple, they should be ordered that the most default is at the top and the most "
                       "changed at the bottom."));

                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Output Variant Sets"));

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                if (ImGui::BeginListBox("##StaticWords", ImGui::GetContentRegionAvail())) {
                    for (auto w : wordsStatic) {
                        bool selected = false;
                        ImGui::Selectable(analyzeResults.mapWordStrings[w].c_str(), selected);
                        MakeTooltip(analyzeResults.mapWordItems[w].strItemList.c_str(), true);

                        DragDropWords::Source(w);
                    }

                    ImGui::EndListBox();
                    DragDropWords::Target(&wordsStatic);
                }

                ImGui::TableNextColumn();
                if (ImGui::BeginListBox("##UnsetWords", ImGui::GetContentRegionAvail())) {
                    for (int i = 0; i <= nShowWords; i++) {
                        for (auto w : analyzeResults.sets[i]) {
                            if (wordsUsed.contains(w)) continue;

                            bool selected = false;
                            ImGui::Selectable(analyzeResults.mapWordStrings[w].c_str(), selected);
                            MakeTooltip(analyzeResults.mapWordItems[w].strItemList.c_str(), true);

                            DragDropWords::Source(w);
                        }
                    }

                    ImGui::Separator();

                    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::Button(LZ("Show More Words"))) {
                        nShowWords = std::min(nShowWords + 1, AnalyzeResults::eWords_Count - 1);
                    }

                    if (ImGui::Button(LZ("Show Less Words"))) {
                        nShowWords = std::max(nShowWords - 1, (int)AnalyzeResults::eWords_StaticVariants);
                    }
                    ImGui::PopItemWidth();
                    ImGui::EndListBox();

                    DragDropWords::Target(nullptr);
                }

                ImGui::TableNextColumn();
                for (const auto& dv : g_Config.mapDynamicVariants) {
                    if (ImGui::CollapsingHeader(dv.first.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet)) {
                        DragDropWords::Target(&mapDVWords[&dv.second], 0);

                        auto copy = mapDVWords[&dv.second];  // Drag drop can modify
                        for (auto it = copy.begin(); it != copy.end(); it++) {
                            // bool selected = false
                            auto w = *it;
                            if (ImGui::TreeNodeEx(analyzeResults.mapWordStrings[w].c_str(), ImGuiTreeNodeFlags_Leaf)) {
                                MakeTooltip(analyzeResults.mapWordItems[w].strItemList.c_str(), true);

                                DragDropWords::Source(w);
                                DragDropWords::Target(&mapDVWords[&dv.second], w);

                                ImGui::TreePop();
                            }
                        }
                    }
                }

                if (bDVChanged) data.dvSets = MapVariants(analyzeResults, mapDVWords);

                ImGui::TableNextColumn();

                ImGui::BeginChild("OutputTree", ImGui::GetContentRegionAvail());

                for (const auto& set : data.dvSets) {
                    if (set.second.empty()) continue;

                    if (ImGui::TreeNodeEx(set.first->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        for (const auto& i : set.second) {
                            if (ImGui::TreeNodeEx(i.second[0]->GetName(), ImGuiTreeNodeFlags_DefaultOpen)) {
                                for (int j = 1; j < i.second.size(); j++)
                                    if (ImGui::TreeNodeEx(i.second[j]->GetName(), ImGuiTreeNodeFlags_Leaf)) {
                                        ImGui::TreePop();
                                    }
                                ImGui::TreePop();
                            }
                        }
                        ImGui::TreePop();
                    }
                }

                ImGui::EndChild();

                ImGui::EndTable();
            }

            ImGui::EndPopup();
        } else {
            if (dvWndWasOpen) {
                dvWndWasOpen = false;
            }
        }

        using ItemGroup = std::map<std::string, std::vector<RE::TESBoundObject*>>;
        static std::vector<std::pair<std::string, ItemGroup>> itemCats;
        static std::unordered_set<RE::TESBoundObject*> selected;

        static RE::TESBoundObject* itemClickedLast = nullptr;
        static GivenItems itemsCustomTemp;

        static char filenameKIDExport[200] = "";
        static char filenameSkypatcherExport[200] = "";

        static std::size_t nMaxCatRows = 0;

        enum { eCatArmor0 = 0, eCatWeapons = eCatArmor0 + 32, eCatAmmo, eCatArmorNoSlots, eCatCount };
        if (popupCustomKeywords) {
            static bool bInit = false;
            if (!bInit) {
                bInit = true;

                std::map<RE::TESFile*, std::map<RE::BGSKeyword*, uint64_t>> mapKeywordDist;

                auto dataHandler = RE::TESDataHandler::GetSingleton();

                for (auto i : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
                    for (unsigned int kw = 0; kw < i->numKeywords; kw++) {
                        auto slots = (ArmorSlots)i->GetSlotMask();
                        if (slots)
                            mapKeywordDist[i->GetFile(0)][i->keywords[kw]] |= slots;
                        else
                            mapKeywordDist[i->GetFile(0)][i->keywords[kw]] |= (1ull << eCatArmorNoSlots);
                    }
                }

                for (auto i : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
                    for (unsigned int kw = 0; kw < i->numKeywords; kw++) {
                        mapKeywordDist[i->GetFile(0)][i->keywords[kw]] |= (1ull << eCatWeapons);
                    }
                }

                for (auto ammo : dataHandler->GetFormArray<RE::TESAmmo>()) {
                    auto i = ammo->AsKeywordForm();
                    for (unsigned int kw = 0; kw < i->numKeywords; kw++) {
                        mapKeywordDist[ammo->GetFile(0)][i->keywords[kw]] |= (1ull << eCatAmmo);
                    }
                }

                struct KWData {
                    int count = 0;
                    int appearance[eCatCount] = {0};
                };

                std::map<RE::BGSKeyword*, KWData> kwTotals;

                for (auto& i : mapKeywordDist) {
                    for (auto& kw : i.second) {
                        auto& kwdata = kwTotals[kw.first];
                        kwdata.count++;

                        auto slots = kw.second;
                        while (slots) {
                            unsigned long slot;
                            _BitScanForward64(&slot, slots);
                            slots &= slots - 1;
                            kwdata.appearance[slot]++;
                        }
                    }
                }

                for (auto& i : kwTotals) {
                    if (!i.second.count) continue;

                    auto it = g_Config.mapCustomKWs.find(i.first);
                    if (it != g_Config.mapCustomKWs.end()) {
                        auto& ckw = it->second;
                        auto min = std::max(5, i.second.count / 10);
                        for (int slot = 0; slot < eCatCount; slot++) {
                            if (i.second.appearance[slot] >= min) ckw.commonSlots |= (1ull << slot);
                        }
                    }
                }
            }

            ImGui::OpenPopup(LZ("Custom Keywords"));

            itemClickedLast = nullptr;

            itemCats.clear();
            itemCats.resize(eCatCount);

            for (int i = 0; i < 32; i++) {
                itemCats[eCatArmor0 + i].first = strSlotDesc[i];
            }
            itemCats[eCatArmorNoSlots].first = LZ("Armor - Unassigned slot");
            itemCats[eCatWeapons].first = LZ("Weapons");
            itemCats[eCatAmmo].first = LZ("Ammo");

            auto itemGroups = GroupItems(data.items, analyzeResults);
            for (auto& i : itemGroups) {
                auto item = i.second[0];
                ItemGroup* pGroup = nullptr;
                if (auto armor = item->As<RE::TESObjectARMO>()) {
                    auto slots = (ArmorSlots)armor->GetSlotMask();
                    if (slots)
                        pGroup = &itemCats[eCatArmor0 + GetSlotIndex(slots)].second;
                    else
                        pGroup = &itemCats[eCatArmorNoSlots].second;
                } else if (auto weapon = item->As<RE::TESObjectWEAP>()) {
                    pGroup = &itemCats[eCatWeapons].second;
                } else if (auto ammo = item->As<RE::TESAmmo>()) {
                    pGroup = &itemCats[eCatAmmo].second;
                }

                if (pGroup) pGroup->emplace(std::move(i.first), std::move(i.second));
            }

            nMaxCatRows = 0;
            for (auto& i : itemCats) nMaxCatRows = std::max(nMaxCatRows, i.second.size());

            selected.clear();

            if (params.mapKeywordChanges.empty()) params.mapKeywordChanges = LoadKeywordChanges(params);

            filenameKIDExport[0] = '\0';
            if (!data.filteredItems.empty()) {
                strncpy(filenameKIDExport, std::format("{} QAR_Export", data.filteredItems[0]->GetFile(0)->fileName).c_str(), sizeof(filenameKIDExport) - 1);
                filenameKIDExport[sizeof(filenameKIDExport) - 1] = '\0';

                strncpy(filenameSkypatcherExport, std::format("QAR Keyword Export\\{}", data.filteredItems[0]->GetFile(0)->fileName).c_str(), sizeof(filenameSkypatcherExport) - 1);
                filenameSkypatcherExport[sizeof(filenameSkypatcherExport) - 1] = '\0';
            }
        }

        ImGui::SetNextWindowSizeConstraints({450, 300}, {1600, 1000});
        // ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;

        static bool bPopupKeywordsWasOpen = false;

        if (ImGui::BeginPopupModal(LZ("Custom Keywords"), &bPopupActive, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar)) {
            bPopupKeywordsWasOpen = true;
            itemsCustomTemp.recentEquipSlots = 0;

            static float fMaxKWSize = 0;

            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu(LZ("Import Keywords"))) {
                    static RE::TESFile* curKWFile = nullptr;
                    static std::map<RE::TESFile*, std::vector<RE::BGSKeyword*>> mapFileKeywords;
                    static std::vector<RE::TESFile*> lsSortedFiles;
                    static std::unordered_map<RE::BGSKeyword*, bool> mapEnabledKWs;

                    if (mapFileKeywords.empty()) {
                        auto dataHandler = RE::TESDataHandler::GetSingleton();
                        for (auto kw : dataHandler->GetFormArray<RE::BGSKeyword>()) {
                            mapFileKeywords[kw->GetFile(0)].push_back(kw);
                        }

                        mapFileKeywords.erase(nullptr);  // Discard dynamic keywords

                        for (auto& ls : mapFileKeywords) {
                            std::sort(ls.second.begin(), ls.second.end(),
                                      [](RE::BGSKeyword* const a, RE::BGSKeyword* const b) { return _stricmp(a->formEditorID.c_str(), b->formEditorID.c_str()) < 0; });

                            lsSortedFiles.push_back(ls.first);
                        }

                        std::sort(lsSortedFiles.begin(), lsSortedFiles.end(), [](RE::TESFile* const a, RE::TESFile* const b) { return _stricmp(a->fileName, b->fileName) < 0; });

                        for (auto& i : g_Config.mapCustomKWs) {
                            mapEnabledKWs[i.first] = true;
                        }
                    }

                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::BeginCombo("##Mod", curKWFile ? curKWFile->fileName : LZ("Select a mod"), ImGuiComboFlags_HeightLarge)) {
                        for (auto file : lsSortedFiles) {
                            if (ImGui::Selectable(file->fileName, file == curKWFile)) {
                                curKWFile = file;
                            }
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::Text(LZ("Tab (optional)"));
                    ImGui::SameLine();
                    static char strTab[64] = "";
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::InputText("##TabName", strTab, sizeof(strTab) - 1);

                    if (ImGui::BeginListBox("##KeywordList", ImVec2(300.0f, 500.0f))) {
                        if (ImGui::BeginPopupContextWindow()) {
                            if (ImGui::Selectable(LZ("Enable all"))) {
                                for (auto kw : mapFileKeywords[curKWFile]) mapEnabledKWs[kw] = true;
                            }
                            if (ImGui::Selectable(LZ("Disable all"))) {
                                for (auto kw : mapFileKeywords[curKWFile]) mapEnabledKWs[kw] = false;
                            }
                            ImGui::EndPopup();
                        }

                        for (auto kw : mapFileKeywords[curKWFile]) {
                            if (ImGui::Checkbox(kw->formEditorID.c_str(), &mapEnabledKWs[kw])) {
                            }
                        }

                        ImGui::EndListBox();
                    }

                    static TimedTooltip resp;
                    if (ImGui::Button(RightAlign(LZ("Import")))) {
                        std::set<RE::BGSKeyword*> kws;

                        // auto& tab = mapKWTabs[strTab];
                        for (auto kw : mapFileKeywords[curKWFile]) {
                            if (mapEnabledKWs[kw]) {
                                kws.insert(kw);
                                fMaxKWSize = std::max(fMaxKWSize, 25.0f + ImGui::CalcTextSize(kw->formEditorID.c_str()).x);
                            }
                        }

                        ImportKeywords(curKWFile, strTab, kws);
                        resp.Enable(LZ("Keywords imported."));
                    }
                    if (!resp.Show())
                        MakeTooltip(
                            LZ("Creates a basic config file for the selected keywords.\n"
                               "Advanced configuration is available by editing the generated file directly."));

                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu(LZ("Export Changes"))) {
                    if (ImGui::BeginMenu(LZ("to Keyword Item Distributor (KID)"))) {
                        ImGui::Text(LZ("This will export all keyword additions for currently displayed items to KID."));
                        ImGui::Text(LZ("Note: KID only supports the addition of keywords, not any removals."));

                        ImGui::Text(LZ("File name:"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(340.0f);
                        ImGui::InputText("##Filename", filenameKIDExport, sizeof(filenameKIDExport));
                        ImGui::SameLine();
                        ImGui::Text("_KID.ini");

                        std::string filename(std::format("{}_KID.ini", filenameKIDExport));
                        std::filesystem::path path = std::filesystem::current_path() / "Data" / filename;

                        if (std::filesystem::exists(path)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: File exists, contents will be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }

                        ImGui::BeginDisabled(!*filenameKIDExport);
                        static TimedTooltip resp;
                        if (ImGui::Button(RightAlign(LZ("Export Changes##Button")))) {
                            if (ExportToKID(data.filteredItems, params.mapKeywordChanges, path))
                                resp.Enable(LZ("Exported succesfully"));
                            else
                                resp.Enable(LZFormat("Could not open file to write {}:\n\n{}", path.generic_string(), std::strerror(errno)));
                        }
                        resp.Show();
                        ImGui::EndDisabled();

                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu(LZ("to Skypatcher"))) {
                        ImGui::Text(LZ("This will create multiple file(s) for SkyPatcher to use."));
                        ImGui::Text(LZ("It is recommended to export to a subdirectory."));
                        ImGui::Text(LZ("File name(s):"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(420.0f);
                        ImGui::InputText("##Filename", filenameSkypatcherExport, sizeof(filenameSkypatcherExport));
                        ImGui::SameLine();
                        ImGui::Text(".ini");

                        std::filesystem::path filename(std::format("{}.ini", filenameSkypatcherExport));
                        std::filesystem::path pathBase = std::filesystem::current_path() / "Data/SKSE/Plugins/SkyPatcher/";

                        if (std::filesystem::exists(pathBase / "armor" / filename)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: Armor file exists, contents may be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }
                        if (std::filesystem::exists(pathBase / "weapon" / filename)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: Weapon file exists, contents may be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }
                        if (std::filesystem::exists(pathBase / "ammo" / filename)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: Ammo file exists, contents may be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }

                        ImGui::BeginDisabled(!*filenameSkypatcherExport);
                        static TimedTooltip resp;
                        if (ImGui::Button(RightAlign(LZ("Export Changes##Button")))) {
                            if (ExportToSkypatcher(data.filteredItems, params.mapKeywordChanges, filename))
                                resp.Enable(LZ("Exported succesfully"));
                            else
                                resp.Enable(LZ("Unable to export to Skypatcher, see logs for details"));
                        }
                        resp.Show();
                        ImGui::EndDisabled();

                        ImGui::EndMenu();
                    }

                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu(LZ("Options"))) {
                    ImGui::MenuItem(LZ("Show Types and Slots"), nullptr, &g_Config.bShowKeywordSlots);
                    ImGui::MenuItem(LZ("Reorder keywords based on relevance"), nullptr, &g_Config.bReorderKeywordsForRelevance);
                    if (ImGui::MenuItem(LZ("Equip example items"), nullptr, &g_Config.bEquipPreviewForKeywords)) {
                        if (!g_Config.bEquipPreviewForKeywords) {
                            itemsCustomTemp.Pop(true);
                            itemsCustomTemp.Restore();
                        }
                    }

                    ImGui::EndMenu();
                }

                ImGui::EndMenuBar();
            }

            static TimedTooltip resp;
            if (ImGui::Button(RightAlign(LZ("Save changes")))) {
                MakeKeywordChanges(params, true);
                resp.Enable(LZ("Changes saved"));
            }
            if (!resp.Show()) MakeTooltip(LZ("Keyword changes are also saved with the Apply Changes button."));

            if (ImGui::BeginTable("WindowTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn(LZ("Items"), 0, 100.0f);
                ImGui::TableSetupColumn(LZ("Keywords"), ImGuiTableColumnFlags_WidthStretch, 300.0f);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                if (ImGui::BeginChild("ItemTree")) {
                    static bool bSkipHeaders;
                    static ArmorChangeParams* pParams;

                    pParams = &params;
                    bSkipHeaders = !g_Config.bShowKeywordSlots || nMaxCatRows < 2;
                    struct ItemTree {
                        bool IsItemChanged(RE::TESBoundObject* item) {
                            KeywordChangeMap& mapChanges = pParams->mapKeywordChanges;
                            for (auto& kwc : mapChanges) {
                                if (kwc.second.add.contains(item) || kwc.second.remove.contains(item)) return true;
                            }
                            return false;
                        }

                        bool IsAllSelected(const ItemGroup::value_type& group) {
                            for (auto i : group.second) {
                                if (!selected.contains(i)) {
                                    return false;
                                }
                            }

                            return true;
                        }

                        void ItemLeaf(RE::TESBoundObject* item) {
                            int nPopColor = 0;
                            if (IsItemChanged(item)) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                nPopColor++;
                            }
                            auto bOpen = ImGui::TreeNodeEx(item->GetName(), (selected.contains(item) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_Leaf);

                            if (ImGui::IsItemClicked()) {
                                itemClicked = item;
                            }

                            ImGui::PopStyleColor(nPopColor);

                            if (bOpen) {
                                ImGui::TreePop();
                            }
                        }

                        void ItemGroup(const ItemGroup::value_type& group) {
                            bool bAllChanged = true;
                            bool bAnyChanged = false;

                            for (auto i : group.second) {
                                if (IsItemChanged(i)) {
                                    bAnyChanged = true;
                                    continue;
                                }
                                bAllChanged = false;
                            }

                            int nPopColor = 0;
                            if (bAllChanged) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                nPopColor++;
                            } else if (bAnyChanged) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChangedPartial);
                                nPopColor++;
                            }

                            auto bOpen = ImGui::TreeNodeEx(group.first.c_str(), (IsAllSelected(group) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow);

                            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                                groupClicked = &group;
                            }

                            ImGui::PopStyleColor(nPopColor);

                            if (bOpen) {
                                for (auto i : group.second) {
                                    ItemLeaf(i);
                                }
                                ImGui::TreePop();
                            }
                        }

                        void Build() {
                            for (auto& cat : itemCats) {
                                if (cat.second.empty()) continue;

                                if (bSkipHeaders || ImGui::CollapsingHeader(cat.first.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_DefaultOpen)) {
                                    for (auto& i : cat.second) {
                                        if (i.second.size() > 1) {
                                            ItemGroup(i);
                                        } else {
                                            ItemLeaf(i.second[0]);
                                        }
                                    }
                                }
                            }
                        }

                        bool HandleSelection() {
                            if (!itemClicked && !groupClicked) return false;

                            const bool isShiftDown = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                            const bool isCtrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                            // const bool isAltDown = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);

                            if (!isCtrlDown) selected.clear();

                            if (!isShiftDown) {
                                if (itemClicked) {
                                    if (!selected.contains(itemClicked)) {
                                        selected.insert(itemClicked);
                                        itemClickedLast = itemClicked;
                                    } else
                                        selected.erase(itemClicked);
                                } else if (groupClicked) {
                                    if (!IsAllSelected(*groupClicked)) {
                                        for (auto i : groupClicked->second) selected.insert(i);

                                        itemClickedLast = groupClicked->second[0];
                                    } else {
                                        for (auto i : groupClicked->second) selected.erase(i);
                                    }
                                }
                            } else {
                                if (groupClicked) itemClicked = groupClicked->second[0];

                                bool bSelecting = false;
                                bool bDone = false;
                                for (auto& cat : itemCats) {
                                    for (auto& i : cat.second) {
                                        for (auto item : i.second) {
                                            if (item == itemClicked || item == itemClickedLast) {
                                                if (!bSelecting) {
                                                    bSelecting = true;
                                                } else {
                                                    bDone = true;
                                                    if (!groupClicked) {  // Need to distinguish between clicking on the group and clicking on the first item
                                                        selected.insert(item);
                                                        break;
                                                    }
                                                }
                                            }

                                            if (bSelecting) selected.insert(item);
                                        }
                                        if (bDone) break;
                                    }
                                    if (bDone) break;
                                }
                            }

                            return true;
                        }

                        RE::TESBoundObject* itemClicked = nullptr;
                        const ItemGroup::value_type* groupClicked = nullptr;
                    };

                    ItemTree itemTree;
                    itemTree.Build();
                    if (itemTree.HandleSelection()) {
                        if (g_Config.bEquipPreviewForKeywords) {
                            if (itemsCustomTemp.stored.empty()) itemsCustomTemp.UnequipCurrent();

                            itemsCustomTemp.Pop(true);
                            for (auto i : selected) {
                                if (auto armor = i->As<RE::TESObjectARMO>()) {
                                    if ((itemsCustomTemp.recentEquipSlots & (ArmorSlots)armor->GetSlotMask()) == 0) itemsCustomTemp.Give(armor, true);
                                }
                            }
                        }
                    }
                }
                ImGui::EndChild();

                ImGui::TableNextColumn();

                if (g_Config.mapCustomKWTabs.empty())
                    ImGui::Text(LZ("No keywords, import some to begin."));
                else {
                    if (fMaxKWSize == 0) {
                        for (auto& tab : g_Config.mapCustomKWTabs) {
                            for (auto i : tab.second) {
                                fMaxKWSize = std::max(fMaxKWSize, 25.0f + ImGui::CalcTextSize(g_Config.mapCustomKWs[i].name.c_str()).x);
                            }
                        }
                    }

                    uint64_t slotsUsed = 0;
                    for (auto i : selected) {
                        if (auto armor = i->As<RE::TESObjectARMO>()) {
                            auto slots = (ArmorSlots)armor->GetSlotMask();
                            if (slots)
                                slotsUsed |= slots;
                            else
                                slotsUsed |= (1ull << eCatArmorNoSlots);
                        } else if (auto weapon = i->As<RE::TESObjectWEAP>())
                            slotsUsed |= (1ull << eCatWeapons);
                        else if (auto ammo = i->As<RE::TESAmmo>())
                            slotsUsed |= (1ull << eCatAmmo);
                    }

                    if (ImGui::BeginTabBar("##KWTabs")) {
                        for (auto& tab : g_Config.mapCustomKWTabs) {
                            if (ImGui::BeginTabItem(tab.first.empty() ? LZ("General") : tab.first.c_str())) {
                                int nCols = std::clamp((int)(ImGui::GetContentRegionAvail().x / fMaxKWSize), 1, 8);

                                if (ImGui::BeginTable("Keywords Table", nCols, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY,
                                                      ImGui::GetContentRegionAvail())) {
                                    ImGui::TableNextRow();

                                    ImGui::BeginDisabled(selected.empty());

                                    for (int priority = g_Config.bReorderKeywordsForRelevance ? 1 : 0; priority >= 0; priority--) {
                                        bool bUsed = false;
                                        KeywordChangeMap& mapChanges = params.mapKeywordChanges;
                                        for (int i = 0; i < tab.second.size(); i++) {
                                            auto& ckw = g_Config.mapCustomKWs[tab.second[i]];
                                            if (g_Config.bReorderKeywordsForRelevance ? (!!priority == !!(ckw.commonSlots & slotsUsed)) : !priority) {
                                                bUsed = true;
                                                ImGui::TableNextColumn();

                                                auto& changes = mapChanges[ckw.kw];

                                                int nChanges = 0;
                                                int nState = (1 << 1);
                                                if (selected.empty())
                                                    nState = 0;
                                                else {
                                                    for (auto item : selected) {
                                                        if (auto form = item->As<RE::BGSKeywordForm>()) {
                                                            if (changes.add.contains(item)) {
                                                                nState |= (1 << 0);
                                                                nChanges |= (1 << 0);
                                                            } else if (changes.remove.contains(item)) {
                                                                nState &= ~(1 << 1);
                                                                nChanges |= (1 << 1);
                                                            } else if (form->HasKeyword(ckw.kw)) {
                                                                nState |= (1 << 0);
                                                            } else
                                                                nState &= ~(1 << 1);
                                                            if (nState == (1 << 0)) break;
                                                        }
                                                    }
                                                }

                                                int nPopColor = 0;

                                                constexpr ImU32 colChange[] = {IM_COL32(255, 255, 255, 255), IM_COL32(0, 255, 0, 255), IM_COL32(255, 0, 0, 255),
                                                                               IM_COL32(255, 255, 0, 255)};
                                                if (nChanges) {
                                                    nPopColor++;
                                                    ImGui::PushStyleColor(ImGuiCol_Text, colChange[nChanges]);
                                                }

                                                if (ImGui::CheckboxFlags(ckw.name.c_str(), &nState, 3)) {
                                                    static void (*RemoveKW)(KeywordChangeMap&, RE::TESBoundObject*, const CustomKeyword&,
                                                                            std::set<RE::BGSKeyword*>&) = [](KeywordChangeMap& changeMap, RE::TESBoundObject* item,
                                                                                                             const CustomKeyword& ckw, std::set<RE::BGSKeyword*>& touched) -> void {
                                                        if (touched.contains(ckw.kw)) return;
                                                        touched.insert(ckw.kw);

                                                        auto& changes = changeMap[ckw.kw];
                                                        if (changes.add.contains(item))
                                                            changes.add.erase(item);
                                                        else {
                                                            if (item->As<RE::BGSKeywordForm>()->HasKeyword(ckw.kw)) changes.remove.insert(item);
                                                        }
                                                    };

                                                    static void (*AddKW)(KeywordChangeMap&, RE::TESBoundObject*, const CustomKeyword&,
                                                                         std::set<RE::BGSKeyword*>&) = [](KeywordChangeMap& changeMap, RE::TESBoundObject* item,
                                                                                                          const CustomKeyword& ckw, std::set<RE::BGSKeyword*>& touched) -> void {
                                                        if (touched.contains(ckw.kw)) return;
                                                        touched.insert(ckw.kw);

                                                        auto& changes = changeMap[ckw.kw];
                                                        if (changes.remove.contains(item))
                                                            changes.remove.erase(item);
                                                        else
                                                            changes.add.insert(item);

                                                        for (auto i : ckw.imply) {
                                                            auto it = g_Config.mapCustomKWs.find(i);
                                                            if (it != g_Config.mapCustomKWs.end()) AddKW(changeMap, item, it->second, touched);
                                                        }

                                                        for (auto i : ckw.exclude) {
                                                            auto it = g_Config.mapCustomKWs.find(i);
                                                            if (it != g_Config.mapCustomKWs.end()) RemoveKW(changeMap, item, it->second, touched);
                                                        }
                                                    };

                                                    for (auto item : selected) {
                                                        std::set<RE::BGSKeyword*> touched;
                                                        if (auto form = item->As<RE::BGSKeywordForm>()) {
                                                            if (nState) {
                                                                AddKW(params.mapKeywordChanges, item, ckw, touched);
                                                            } else {
                                                                RemoveKW(params.mapKeywordChanges, item, ckw, touched);
                                                            }
                                                        }
                                                    }
                                                }

                                                if (!ckw.tooltip.empty()) MakeTooltip(ckw.tooltip.c_str());

                                                ImGui::PopStyleColor(nPopColor);
                                            }
                                        }

                                        if (bUsed && priority) {
                                            ImGui::TableNextRow();
                                            for (int n = 0; n < nCols; n++) {
                                                ImGui::TableNextColumn();
                                                ImGui::Separator();
                                            }
                                            ImGui::TableNextRow();
                                        }
                                    }

                                    ImGui::EndDisabled();

                                    ImGui::EndTable();
                                }

                                ImGui::EndTabItem();
                            }
                        }

                        ImGui::EndTabBar();
                    }
                }

                ImGui::EndTable();
            }

            ImGui::EndPopup();
        } else if (bPopupKeywordsWasOpen) {
            bPopupKeywordsWasOpen = false;
            itemsCustomTemp.Pop(true);
            itemsCustomTemp.Restore();
        }
        if (switchToMod) Local::SwitchToMod(switchToMod);
    }

    ImGuiIntegration::BlockInput(!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsWindowCollapsed(), ImGui::IsItemHovered());
    ImGui::End();

    if (g_Config.bExportUntranslated) Localization::Get()->Export();
    if (!isActive) ImGuiIntegration::Show(false);
}