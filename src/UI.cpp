#include "UI.h"

#include "ArmorChanger.h"
#include "ArmorSetBuilder.h"
#include "Config.h"
#include "Data.h"
#include "ImGuiIntegration.h"

using namespace QuickArmorRebalance;

constexpr int kItemListLimit = 2000;
constexpr int kItemApplyWarningThreshhold = 100;

const char* strSlotDesc[] = {"Slot 30 - Head",       "Slot 31 - Hair",       "Slot 32 - Body",
                             "Slot 33 - Hands",      "Slot 34 - Forearms",   "Slot 35 - Amulet",
                             "Slot 36 - Ring",       "Slot 37 - Feet",       "Slot 38 - Calves",
                             "Slot 39 - Shield",     "Slot 40 - Tail",       "Slot 41 - Long Hair",
                             "Slot 42 - Circlet",    "Slot 43 - Ears",       "Slot 44 - Face",
                             "Slot 45 - Neck",       "Slot 46 - Chest",      "Slot 47 - Back",
                             "Slot 48 - ???",        "Slot 49 - Pelvis",     "Slot 50 - Decapitated Head",
                             "Slot 51 - Decapitate", "Slot 52 - Lower body", "Slot 53 - Leg (right)",
                             "Slot 54 - Leg (left)", "Slot 55 - Face2",      "Slot 56 - Chest2",
                             "Slot 57 - Shoulder",   "Slot 58 - Arm (left)", "Slot 59 - Arm (right)",
                             "Slot 60 - ???",        "Slot 61 - ???",        "<REMOVE SLOT>"};

static ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return {a.x + b.x, a.y + b.y}; }
static ImVec2 operator/(const ImVec2& a, int b) { return {a.x / b, a.y / b}; }

bool StringContainsI(const char* s1, const char* s2) {
    std::string str1(s1), str2(s2);
    std::transform(str1.begin(), str1.end(), str1.begin(), ::tolower);
    std::transform(str2.begin(), str2.end(), str2.begin(), ::tolower);
    return str1.contains(str2);
}

void MakeTooltip(const char* str, bool delay = false) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | (delay ? ImGuiHoveredFlags_DelayNormal : 0)))
        ImGui::SetTooltip(str);
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

        if (bUnmodified && (g_Data.modifiedItems.contains(obj) || g_Data.modifiedItemsShared.contains(obj)))
            return false;

        return true;
    }
};

void RightAlign(const char* text) {
    float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
}

struct GivenItems {
    void UnequipCurrent() {
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            for (auto& item : player->GetInventory()) {
                if (item.second.second->IsWorn() && item.first->IsArmor()) {
                    if (auto i = item.first->As<RE::TESObjectARMO>()) {
                        if (((unsigned int)i->GetSlotMask() & g_Config.usedSlotsMask) ==
                            0) {  // Not a slot we interact with - probably physics or such
                            continue;
                        }

                        RE::ActorEquipManager::GetSingleton()->UnequipObject(player, i, nullptr, 1, i->GetEquipSlot(),
                                                                             false, false, false);
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
                SKSE::GetTaskInterface()->AddTask([=]() {
                    RE::ActorEquipManager::GetSingleton()->EquipObject(player, item, nullptr, 1, armor->GetEquipSlot(),
                                                                       false, false, false);
                });
            }
        } else {
            SKSE::GetTaskInterface()->AddTask(
                [=]() { RE::ActorEquipManager::GetSingleton()->EquipObject(player, item); });
        }
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

    void Pop() {
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        if (items.empty()) return;
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto t = items.back().first;

        while (!items.empty() && items.back().first == t) {
            player->RemoveItem(items.back().second, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            items.pop_back();
        }
    }

    unsigned int recentEquipSlots = 0;
    std::vector<std::pair<int, RE::TESBoundObject*>> items;
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

void SliderRow(const char* field, ArmorChangeParams::SliderPair& pair, float min = 0.0f, float max = 300.0f,
               float def = 100.0f) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(field);
    ImGui::Checkbox(std::format("Modify {}", field).c_str(), &pair.bModify);
    ImGui::BeginDisabled(!pair.bModify);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::SliderFloat("##Scale", &pair.fScale, min, max, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TableNextColumn();
    if (ImGui::Button("Reset")) pair.fScale = def;
    ImGui::EndDisabled();
    ImGui::PopID();
}

void PermissionsChecklist(const char* id, Permissions& p) {
    ImGui::PushID(id);
    ImGui::Checkbox("Distribute loot", &p.bDistributeLoot);
    ImGui::Checkbox("Modify keywords", &p.bModifyKeywords);
    ImGui::Checkbox("Modify armor slots", &p.bModifySlots);
    ImGui::Checkbox("Modify armor rating", &p.bModifyArmorRating);
    ImGui::Checkbox("Modify armor weight", &p.bModifyWeight);
    ImGui::Checkbox("Modify armor warmth", &p.bModifyWarmth);
    ImGui::Checkbox("Modify weapon damage", &p.bModifyWeapDamage);
    ImGui::Checkbox("Modify weapon weight", &p.bModifyWeapWeight);
    ImGui::Checkbox("Modify weapon speed", &p.bModifyWeapSpeed);
    ImGui::Checkbox("Modify weapon stagger", &p.bModifyWeapStagger);
    ImGui::Checkbox("Modify value", &p.bModifyValue);
    ImGui::Checkbox("Modify crafting recipes", &p.crafting.bModify);
    ImGui::Checkbox("Create crafting recipes", &p.crafting.bCreate);
    ImGui::Checkbox("Free crafting recipes", &p.crafting.bFree);
    ImGui::Checkbox("Modify temper recipes", &p.temper.bModify);
    ImGui::Checkbox("Create temper recipes", &p.temper.bCreate);
    ImGui::Checkbox("Free temper recipes", &p.temper.bFree);
    ImGui::PopID();
}

enum { ModSpecial_Worn, ModSpecial_All };

void AddFormsToList(const auto& all, const ItemFilter& filter) {
    ArmorChangeParams& params = g_Config.acParams;
    for (auto i : all) {
        if (!IsValidItem(i)) continue;
        if (!filter.Pass(i)) continue;

        if (params.filteredItems.size() < kItemListLimit) params.filteredItems.push_back(i);
    }
}

short g_filterRound = 0;

void GetCurrentListItems(ModData* curMod, int nModSpecial, const ItemFilter& filter, AnalyzeResults& results) {
    static short filterRound = -1;
    if (filterRound == g_filterRound) return;
    filterRound = g_filterRound;

    ArmorChangeParams& params = g_Config.acParams;
    params.filteredItems.clear();
    if (curMod) {
        for (auto i : curMod->items) {
            if (!filter.Pass(i)) continue;

            params.filteredItems.push_back(i);
        }
    } else {
        switch (nModSpecial) {
            case ModSpecial_Worn:
                if (auto player = RE::PlayerCharacter::GetSingleton()) {
                    for (auto& item : player->GetInventory()) {
                        if (item.second.second->IsWorn()) {
                            if (!IsValidItem(item.first)) continue;
                            if (!filter.Pass(item.first)) continue;

                            params.filteredItems.push_back(item.first);
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
    params.dvSets.clear();


    if (!params.filteredItems.empty()) {
        std::sort(params.filteredItems.begin(), params.filteredItems.end(),
                  [](RE::TESBoundObject* const a, RE::TESBoundObject* const b) {
                      return _stricmp(a->GetName(), b->GetName()) < 0;
                  });

        if (curMod) AnalyzeArmor(params.filteredItems, results);
    }
}

bool WillBeModified(RE::TESBoundObject* i, ArmorSlots remapped) {
    if (auto armor = i->As<RE::TESObjectARMO>()) {
        if (((remapped | g_Config.slotsWillChange) & (ArmorSlots)armor->GetSlotMask()) == 0) return false;
    } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
        if (!g_Config.acParams.armorSet->FindMatching(weap)) return false;
    } else if (auto ammo = i->As<RE::TESAmmo>()) {
        if (!g_Config.acParams.armorSet->FindMatching(ammo)) return false;
    } else
        return false;

    return true;
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

void QuickArmorRebalance::RenderUI() {
    const auto colorChanged = IM_COL32(0, 255, 0, 255);
    const auto colorChangedShared = IM_COL32(255, 255, 0, 255);
    const auto colorDeleted = IM_COL32(255, 0, 0, 255);

    const char* rarity[] = {"Common", "Uncommon", "Rare", nullptr};
    const char* itemTypes[] = {"Any", "Armor", "Weapons", "Ammo", nullptr};

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

    static AnalyzeResults analyzeResults;
    static ItemFilter filter;

    static HighlightTrack hlConvert;
    static HighlightTrack hlDistributeAs;
    static HighlightTrack hlRarity;
    static HighlightTrack hlDynamicVariants;
    static HighlightTrack hlSlots;

    auto isInventoryOpen = RE::UI::GetSingleton()->IsItemMenuOpen();

    static bool bMenuHovered = false;
    static bool bSlotWarning = false;
    bool popupSettings = false;
    bool popupRemapSlots = false;
    bool popupDynamicVariants = false;

    ArmorSlots remappedSrc = 0;
    ArmorSlots remappedTar = 0;

    static int nShowWords = AnalyzeResults::eWords_EitherVariants;

    for (auto i : g_Config.acParams.mapArmorSlots) {
        remappedSrc |= 1 << i.first;
        remappedTar |= i.second < 32 ? (1 << i.second) : 0;
    }

    ImGuiWindowFlags wndFlags = ImGuiWindowFlags_NoScrollbar;
    if (bMenuHovered) wndFlags |= ImGuiWindowFlags_MenuBar;

    ImGui::SetNextWindowSizeConstraints({700, 250}, {1600, 1000});
    if (ImGui::Begin("Quick Armor Rebalance", &isActive, wndFlags)) {
        if (g_Config.strCriticalError.empty()) {
            ArmorChangeParams& params = g_Config.acParams;

            const char* strModSpecial[] = {"<Currently Worn Armor>", "<All Items>", nullptr};
            bool bModSpecialEnabled[] = {true, g_Config.bEnableAllItems};
            static int nModSpecial = 0;

            static bool bFilterChangedMods = false;

            if (bMenuHovered) {
                bMenuHovered = false;

                if (ImGui::BeginMenuBar()) {
                    auto menuSize = ImGui::GetItemRectSize();

                    ImGui::SetItemAllowOverlap();

                    RightAlign("Settings");
                    if (ImGui::MenuItem("Settings")) {
                        popupSettings = true;
                    }

                    ImGui::EndMenuBar();

                    // The menu bar doesn't return the right values (but somehow has the right size?), so instead we
                    // work off the last menu item added
                    bMenuHovered |=
                        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);  // Doesn't actually work
                    bMenuHovered |= ImGui::IsMouseHoveringRect(ImGui::GetWindowPos(), ImGui::GetItemRectMax(), false);

                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - menuSize.y);
                }
            }
            bMenuHovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);

            // ImGui::PushItemWidth(-FLT_MIN);
            ImGui::SetNextItemWidth(-FLT_MIN);

            if (ImGui::BeginTable("WindowTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("LeftCol", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("RightCol", ImGuiTableColumnFlags_WidthFixed,
                                        0.25f * ImGui::GetContentRegionAvail().x);

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

                        ImGui::Text("Mod");
                        ImGui::TableNextColumn();

                        RE::TESFile* blacklist = nullptr;

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::BeginCombo("##Mod", curMod ? curMod->mod->fileName : strModSpecial[nModSpecial],
                                              ImGuiComboFlags_HeightLarge)) {
                            {
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

                            for (auto i : g_Data.sortedMods) {
                                bool selected = curMod == i;

                                int pop = 0;
                                if (g_Data.modifiedFiles.contains(i->mod)) {
                                    if (bFilterChangedMods) continue;
                                    if (g_Data.modifiedFilesDeleted.contains(i->mod))
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                    else
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                    pop++;
                                } else if (g_Data.modifiedFilesShared.contains(i->mod)) {
                                    if (bFilterChangedMods) continue;
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                                    pop++;
                                }

                                if (ImGui::Selectable(i->mod->fileName, selected)) {
                                    if (isCtrlDown && isAltDown && g_Data.modifiedFiles.contains(i->mod)) {
                                        showPopup = true;
                                    }

                                    curMod = i;
                                    givenItems.items.clear();
                                    selectedItems.clear();
                                    lastSelectedItem = nullptr;

                                    if (g_Config.bResetSliders) {
                                        params.armor.rating.fScale = 100.0f;
                                        params.armor.weight.fScale = 100.0f;
                                        params.armor.warmth.fScale = 50.0f;
                                        params.weapon.damage.fScale = 100.0f;
                                        params.weapon.speed.fScale = 100.0f;
                                        params.weapon.weight.fScale = 100.0f;
                                        params.weapon.stagger.fScale = 100.0f;
                                        params.value.fScale = 100.0f;
                                    }
                                    if (g_Config.bResetSlotRemap) {
                                        params.mapArmorSlots.clear();
                                    }

                                    g_filterRound++;
                                }
                                if (isCtrlDown && isAltDown && ImGui::IsItemHovered() &&
                                    ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                                    blacklist = i->mod;
                                }

                                if (selected) ImGui::SetItemDefaultFocus();
                                ImGui::PopStyleColor(pop);
                            }

                            ImGui::EndCombo();

                            params.isWornArmor = !curMod;

                            if (blacklist) g_Config.AddUserBlacklist(blacklist);
                        }

                        {
                            const char* popupTitle = "Delete Changes?";
                            if (showPopup) ImGui::OpenPopup(popupTitle);

                            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                                    ImVec2(0.5f, 0.5f));

                            if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text("Delete all changes to %s?", curMod->mod->fileName);
                                ImGui::Text("Changes will not revert until after restarting Skyrim");

                                if (ImGui::Button("Delete changes", ImVec2(120, 0))) {
                                    DeleteAllChanges(curMod->mod);
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SetItemDefaultFocus();
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Hide modified", &bFilterChangedMods);
                        ImGui::EndTable();
                    }

                    ImGui::Separator();

                    ImGui::Text("Filter");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::InputTextWithHint("##ItemFilter", "by Name", filter.nameFilter,
                                                 sizeof(filter.nameFilter) - 1))
                        g_filterRound++;

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);

                    int popCol = 0;
                    if (!filter.nType) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        popCol++;
                    }

                    if (ImGui::BeginCombo("##Typefilter", filter.nType ? itemTypes[filter.nType] : "by Type")) {
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

                    if (ImGui::BeginCombo("##Slotfilter", filter.slots ? "Filtering by Slot" : "by Slot",
                                          ImGuiComboFlags_HeightLargest)) {
                        ImGui::PopStyleColor(popCol);
                        popCol = 0;

                        if (ImGui::Selectable("Clear filter")) {
                            filter.slots = 0;
                            g_filterRound++;
                        }
                        if (ImGui::RadioButton("Any of", &filter.slotMode, ItemFilter::SlotFilterMode::SlotsAny))
                            g_filterRound++;
                        if (ImGui::RadioButton("All of", &filter.slotMode, ItemFilter::SlotFilterMode::SlotsAll))
                            g_filterRound++;
                        if (ImGui::RadioButton("Not", &filter.slotMode, ItemFilter::SlotFilterMode::SlotsNot))
                            g_filterRound++;

                        constexpr auto slotCols = 4;
                        if (ImGui::BeginTable("##SlotFilterTable", slotCols)) {
                            for (int i = 0; i < 32 / slotCols; i++) {
                                for (int j = 0; j < slotCols; j++) {
                                    auto s = i + j * 32 / slotCols;
                                    bool bCheck = filter.slots & (1 << s);
                                    ImGui::TableNextColumn();
                                    if (ImGui::Checkbox(strSlotDesc[s], &bCheck)) {
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
                    if (ImGui::Checkbox("Unmodified", &filter.bUnmodified)) g_filterRound++;

                    if (ImGui::BeginTable("Convert Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("ConvertCol1");
                        ImGui::TableSetupColumn("ConvertCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ConvertCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text("Convert to");
                        ImGui::TableNextColumn();

                        if (!params.armorSet) params.armorSet = &g_Config.armorSets[0];

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

                        int nArmorSet = 0;
                        hlConvert.Push();
                        if (ImGui::BeginCombo("##ConvertTo", params.armorSet->name.c_str(),
                                              ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                            for (auto& i : g_Config.armorSets) {
                                bool selected = params.armorSet == &i;
                                ImGui::PushID(nArmorSet++);
                                if (ImGui::Selectable(i.name.c_str(), selected)) params.armorSet = &i;
                                if (selected) ImGui::SetItemDefaultFocus();
                                MakeTooltip(i.strContents.c_str(), true);
                                ImGui::PopID();
                            }

                            ImGui::EndCombo();
                        }
                        hlConvert.Pop();

                        ImGui::TableNextColumn();

                        ImGui::Checkbox("Merge", &g_Config.acParams.bMerge);
                        MakeTooltip(
                            "When enabled, merge will keep any previous changes not currently being modified.\n"
                            "Enable only the fields you want to be changing.\n"
                            "Disable this to clear all previous changes.");
                        ImGui::SameLine();

                        static HighlightTrack hlApply;
                        hlApply.Push(hlConvert && hlDistributeAs && hlRarity && hlSlots);

                        ImGui::BeginDisabled(params.filteredItems.size() >= kItemListLimit);

                        bool bApply = false;
                        if (ImGui::Button("Apply changes")) {
                            g_Config.Save();

                            if (params.items.size() < kItemApplyWarningThreshhold) {
                                bApply = true;
                            } else
                                ImGui::OpenPopup("###ApplyWarn");
                        }
                        ImGui::EndDisabled();
                        hlApply.Pop();

                        if (ImGui::BeginPopupModal("Warning###ApplyWarn", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text("This will change %d items, are you sure?", params.items.size());

                            if (ImGui::Button("Yes, apply changes")) {
                                bApply = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SetItemDefaultFocus();
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel")) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::EndTable();

                        if (bApply) {
                            MakeArmorChanges(params);

                            if (g_Config.bAutoDeleteGiven) givenItems.Remove();

                            hlConvert.Touch();
                            hlDistributeAs.Touch();
                            hlRarity.Touch();
                            hlSlots.Touch();
                            hlDynamicVariants.Touch();
                        }
                    }

                    GetCurrentListItems(curMod, nModSpecial, filter, analyzeResults);
                    g_Config.slotsWillChange = GetConvertableArmorSlots(params);

                    bool hasEnabledArmor = false;
                    bool hasEnabledWeap = false;

                    if (params.filteredItems.size() < kItemListLimit) {
                        for (auto i : params.filteredItems) {
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
                    ImGui::BeginDisabled(!curMod);

                    // Need to create a dummy table to negate stretching the combo boxes
                    ImGui::Checkbox("Distribute as ", &params.bDistribute);
                    MakeTooltip(
                        "Additions or changes to loot distribution will not take effect until you restart Skyrim");
                    ImGui::BeginDisabled(!params.bDistribute);

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(300);

                    hlDistributeAs.Push(params.bDistribute);

                    if (ImGui::BeginCombo("##DistributeAs", params.distProfile,
                                          ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                        for (auto& i : g_Config.lootProfiles) {
                            bool selected = params.distProfile == i;
                            if (ImGui::Selectable(i.c_str(), selected)) params.distProfile = i.c_str();
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
                    ImGui::Checkbox("As pieces", &params.bDistAsPieces);
                    ImGui::SameLine();
                    ImGui::Checkbox("As whole set", &params.bDistAsSet);
                    MakeTooltip("You will find entire sets (all slots) together");
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!params.bDistAsSet);
                    ImGui::Checkbox("Matching sets", &params.bMatchSetPieces);
                    MakeTooltip(
                        "Attempts to match sets together - for example, if there are green and blue variants, it will\n"
                        "try to distribute only green or only blue parts as a single set");
                    ImGui::EndDisabled();

                    hlDynamicVariants.Push(!analyzeResults.sets[AnalyzeResults::eWords_DynamicVariants].empty() ||
                                           !analyzeResults.sets[AnalyzeResults::eWords_EitherVariants].empty());

                    ImGui::SameLine();
                    if (ImGui::Button("Dynamic Variants")) {
                        popupDynamicVariants = true;
                    }
                    hlDynamicVariants.Pop();

                    ImGui::Unindent(60);

                    ImGui::EndDisabled();
                    ImGui::EndDisabled();

                    // Modifications
                    if (ImGui::BeginChild("ItemTypeFrame", {ImGui::GetContentRegionAvail().x, 120}, false)) {
                        if (ImGui::BeginTabBar("ItemTypeBar")) {
                            static int iTabOpen = 0;
                            int iTab = 0;
                            int iTabSelected = iTabOpen;

                            bool bTabEnabled[] = {hasEnabledArmor && !params.armorSet->items.empty(),
                                                  hasEnabledWeap && !params.armorSet->weaps.empty()};
                            const int nTabCount = 2;

                            bool bForceSelect = false;
                            if (!bTabEnabled[iTabOpen]) {
                                iTabOpen = 0;
                                while (iTabOpen < nTabCount && !bTabEnabled[iTabOpen]) iTabOpen++;
                                if (iTabOpen >= nTabCount) iTabOpen = 0;

                                bForceSelect = true;
                            }

                            const char* tabLabels[] = {"Armor", "Weapons"};
                            // if (iTabOpen != iTabSelected) ImGui::SetTabItemClosed(tabLabels[iTabSelected]);

                            iTabSelected = iTabOpen;

                            // Amor tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(
                                    tabLabels[iTab], nullptr,
                                    bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow("Armor Rating", params.armor.rating);
                                    SliderRow("Weight", params.armor.weight);

                                    // Warmth
                                    {
                                        const char* warmthDesc[] = {
                                            "None",       //<0.1
                                            "Cold",       //<0.2
                                            "Poor",       //<0.3
                                            "Limited",    //<0.4
                                            "Fair",       //<0.5
                                            "Average",    //<0.6
                                            "Good",       //<0.7
                                            "Warm",       //<0.8
                                            "Full",       //<0.9
                                            "Excellent",  //<1.0
                                            "Maximum",    //=1.0
                                        };

                                        const char* coverageDesc[] = {
                                            "None",         //<0.1
                                            "Minimal",      //<0.2
                                            "Poor",         //<0.3
                                            "Limited",      //<0.4
                                            "Fair",         //<0.5
                                            "Average",      //<0.6
                                            "Good",         //<0.7
                                            "Significant",  //<0.8
                                            "Full",         //<0.9
                                            "Excellent",    //<1.0
                                            "Maximum",      //=1.0
                                        };

                                        bool showCoverage =
                                            g_Config.isFrostfallInstalled || g_Config.bShowFrostfallCoverage;

                                        auto& pair = params.armor.warmth;
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();

                                        ImGui::Checkbox("Modify Warmth", &pair.bModify);
                                        MakeTooltip(
                                            "The total warmth of the armor set.\n"
                                            "This number is NOT relative to the base armor set's warmth.");
                                        ImGui::BeginDisabled(!pair.bModify);
                                        ImGui::TableNextColumn();

                                        ImGui::SetNextItemWidth((showCoverage ? 0.5f : 1.0f) *
                                                                ImGui::GetContentRegionAvail().x);
                                        ImGui::SliderFloat("##WarmthSlider", &pair.fScale, 0.f, 100.f, "%.0f%% Warmth",
                                                           ImGuiSliderFlags_AlwaysClamp);
                                        MakeTooltip(warmthDesc[(int)(0.1f * pair.fScale)]);

                                        if (showCoverage) {
                                            ImGui::SameLine();
                                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                                            ImGui::SliderFloat("##CoverageSlider", &params.armor.coverage, 0.f, 100.f,
                                                               "%.0f%% Coverage", ImGuiSliderFlags_AlwaysClamp);
                                            MakeTooltip(coverageDesc[(int)(0.1f * params.armor.coverage)]);
                                        }

                                        ImGui::TableNextColumn();
                                        if (ImGui::Button("Reset")) {
                                            pair.fScale = 50.0f;
                                            params.armor.coverage = 50.0f;
                                        }
                                        ImGui::EndDisabled();
                                    }

                                    ImGui::EndTable();
                                }

                                ImGui::Separator();
                                ImGui::Text("Stat distribution curve");
                                ImGui::SameLine();

                                static auto* curCurve = &g_Config.curves[0];

                                ImGui::SetNextItemWidth(220);
                                if (ImGui::BeginCombo("##Curve", curCurve->first.c_str(),
                                                      ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
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
                                if (ImGui::Button("Remap Slots")) popupRemapSlots = true;
                                hlSlots.Pop();

                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            // Weapon tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(
                                    tabLabels[iTab], nullptr,
                                    bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow("Damage", params.weapon.damage);
                                    SliderRow("Weight", params.weapon.weight);
                                    SliderRow("Speed", params.weapon.speed);
                                    SliderRow("Stagger", params.weapon.stagger);

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
                        SliderRow("Gold Value", params.value);

                        ImGui::EndTable();
                    }

                    ImGui::Checkbox("Modify Keywords", &params.bModifyKeywords);

                    if (ImGui::BeginTable("Crafting Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("CraftCol1");
                        ImGui::TableSetupColumn("CraftCol2");
                        ImGui::TableSetupColumn("CraftCol3");

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Modify Temper Recipe", &params.temper.bModify);
                        ImGui::BeginDisabled(!params.temper.bModify);
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Create recipe if missing##Temper", &params.temper.bNew);
                        MakeTooltip(
                            "If an item being modified lacks an existing temper recipe to modify, checking\n"
                            "this will create one automatically");
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(g_Config.fTemperGoldCostRatio > 0 ? "Cost gold if no recipe to copy##Temper"
                                                                          : "Make free if no recipe to copy##Temper",
                                        &params.temper.bFree);
                        MakeTooltip(
                            "If the item being copied lacks a tempering recipe, checking will remove all\n"
                            "components and conditions to temper the modified item");
                        ImGui::EndDisabled();

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Modify Crafting Recipe", &params.craft.bModify);
                        ImGui::BeginDisabled(!params.craft.bModify);
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Create recipe if missing##Craft", &params.craft.bNew);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "If an item being modified lacks an existing crafting recipe to modify,\n"
                                "checking this will create one automatically");
                        ImGui::TableNextColumn();
                        ImGui::Checkbox(g_Config.fCraftGoldCostRatio > 0 ? "Cost gold if no recipe to copy##Craft"
                                                                         : "Make free if no recipe to copy##Craft",
                                        &params.craft.bFree);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "If the item being copied lacks a crafting recipe, checking will remove all\n"
                                "components and conditions to craft the modified item");
                        ImGui::EndDisabled();

                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();

                ImGui::TableNextColumn();
                if (params.filteredItems.size() >= kItemListLimit) {
                    ImGui::Text(
                        "Too many items.\n"
                        "Limit the amount of items by using the filters.");
                } else {
                    ImGui::Text((std::format("{}", params.filteredItems.size()) + " Items").c_str());

                    auto avail = ImGui::GetContentRegionAvail();
                    avail.y -= ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.y * 2;

                    auto player = RE::PlayerCharacter::GetSingleton();

                    params.items.clear();
                    params.items.reserve(params.filteredItems.size());

                    bool modChangesDeleted = g_Data.modifiedFilesDeleted.contains(curMod->mod);

                    ImGui::PushStyleColor(ImGuiCol_NavHighlight, IM_COL32(0, 255, 0, 255));

                    if (ImGui::BeginListBox("##Items", avail)) {
                        auto xPos = ImGui::GetCursorPosX();

                        if (ImGui::BeginPopupContextWindow()) {
                            if (ImGui::Selectable("Unequip worn items")) {
                                givenItems.UnequipCurrent();
                            }

                            ImGui::Separator();

                            ImGui::BeginDisabled(selectedItems.empty());
                            if (ImGui::BeginMenu("Selected ...")) {
                                if (ImGui::Selectable("Equip")) {
                                    for (auto i : selectedItems) givenItems.Give(i, true, true);
                                }
                                if (ImGui::Selectable("Unequip")) {
                                    for (auto i : selectedItems) givenItems.Unequip(i);
                                }

                                ImGui::Separator();

                                if (ImGui::Selectable("Enable")) {
                                    for (auto i : selectedItems) uncheckedItems.erase(i);
                                }
                                if (ImGui::Selectable("Enable ONLY")) {
                                    uncheckedItems.clear();
                                    uncheckedItems.insert(params.filteredItems.begin(), params.filteredItems.end());
                                    for (auto i : selectedItems) uncheckedItems.erase(i);
                                }
                                if (ImGui::Selectable("Disable")) {
                                    for (auto i : selectedItems) uncheckedItems.insert(i);
                                }

                                ImGui::EndMenu();
                            }
                            ImGui::EndDisabled();

                            if (ImGui::BeginMenu("Select ...")) {
                                if (ImGui::Selectable("Armor")) {
                                    selectedItems.clear();
                                    for (auto i : params.filteredItems) {
                                        if (auto armor = i->As<RE::TESObjectARMO>()) selectedItems.insert(armor);
                                    }
                                }
                                if (ImGui::Selectable("Weapons")) {
                                    selectedItems.clear();
                                    for (auto i : params.filteredItems) {
                                        if (auto weap = i->As<RE::TESObjectWEAP>()) selectedItems.insert(weap);
                                    }
                                }
                                if (ImGui::Selectable("Ammo")) {
                                    selectedItems.clear();
                                    for (auto i : params.filteredItems) {
                                        if (auto ammo = i->As<RE::TESAmmo>()) selectedItems.insert(ammo);
                                    }
                                }

                                ImGui::EndMenu();
                            }

                            ImGui::Separator();
                            if (ImGui::Selectable("Enable all")) uncheckedItems.clear();
                            if (ImGui::Selectable("Disable all"))
                                for (auto i : params.filteredItems) uncheckedItems.insert(i);

                            ImGui::EndPopup();
                        }

                        // Clear out selections that are no longer visible
                        auto lastSelected(std::move(selectedItems));
                        bool hasAnchor = false;
                        for (auto i : params.filteredItems) {
                            if (lastSelected.contains(i)) selectedItems.insert(i);
                            if (i == lastSelectedItem) hasAnchor = true;
                        }
                        if (!hasAnchor) lastSelectedItem = nullptr;

                        for (auto i : params.filteredItems) {
                            int popCol = 0;
                            if (g_Data.modifiedItems.contains(i)) {
                                if (modChangesDeleted)
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                else
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                popCol++;
                            } else if (g_Data.modifiedItemsShared.contains(i)) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                                popCol++;
                            } else if (!WillBeModified(i, remappedSrc)) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                popCol++;
                            }

                            std::string name(i->GetName());

                            if (name.empty()) name = std::format("{}:{:010x}", i->GetFile(0)->fileName, i->formID);

                            static RE::TESBoundObject* keyboardNav = nullptr;

                            ImGui::BeginGroup();

                            bool isChecked = !uncheckedItems.contains(i);
                            ImGui::PushID(name.c_str());

                            if (ImGui::Checkbox("##ItemCheckbox", &isChecked)) {
                                if (!isChecked)
                                    uncheckedItems.insert(i);
                                else
                                    uncheckedItems.erase(i);
                            }
                            if (isChecked) params.items.push_back(i);

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
                                            auto armorSet = BuildSetFrom(i, params.filteredItems);

                                            if (!isCtrlDown) selectedItems.clear();
                                            for (auto piece : armorSet) selectedItems.insert(piece);
                                        }
                                    } else {
                                        if (isAltDown) {
                                            auto armorSet = BuildSetFrom(i, params.filteredItems);
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
                                            for (auto j : params.filteredItems) {
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
                                ImGui::SetScrollFromPosX(
                                    xPos - ImGui::GetCursorPosX(),
                                    1.0f);  // don't know why this is 1.0 instead of 0, but it works

                                auto draw = ImGui::GetWindowDrawList();
                                draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), colorChanged);

                                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_Keypad8)) {
                                    auto it = std::find(params.filteredItems.begin(), params.filteredItems.end(), i);
                                    if (it != params.filteredItems.end() && it != params.filteredItems.begin()) {
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
                                                for (auto j : params.filteredItems) {
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
                                           (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                                            ImGui::IsKeyPressed(ImGuiKey_Keypad2))) {
                                    auto it = std::find(params.filteredItems.begin(), params.filteredItems.end(), i);
                                    if (it != params.filteredItems.end() && it != params.filteredItems.end() - 1) {
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
                                                for (auto j : params.filteredItems) {
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

                            ImGui::PopStyleColor(popCol);
                            ImGui::PopStyleVar(popVar);
                        }

                        ImGui::EndListBox();
                    }

                    ImGui::PopStyleColor();

                    ImGui::BeginDisabled(!player || !curMod || isInventoryOpen);

                    if (ImGui::Button(selectedItems.empty() ? "Give All" : "Give Selected")) {
                        if (selectedItems.empty())
                            for (auto i : params.filteredItems) {
                                givenItems.Give(i);
                            }
                        else
                            for (auto i : selectedItems) {
                                givenItems.Give(i);
                            }
                    }
                    if (isInventoryOpen) MakeTooltip("Can't use while inventory is open");

                    ImGui::SameLine();
                    if (ImGui::Button(selectedItems.empty() ? "Equip All" : "Equip Selected")) {
                        givenItems.UnequipCurrent();
                        if (selectedItems.empty())
                            for (auto i : params.filteredItems) {
                                givenItems.Give(i, true);
                            }
                        else
                            for (auto i : selectedItems) {
                                givenItems.Give(i, true);
                            }
                    }
                    if (isInventoryOpen) MakeTooltip("Can't use while inventory is open");

                    ImGui::BeginDisabled(givenItems.items.empty());
                    ImGui::SameLine();
                    if (ImGui::Button("Delete Given")) {
                        givenItems.Remove();
                    }
                    if (isInventoryOpen) MakeTooltip("Can't use while inventory is open");

                    ImGui::EndDisabled();  // givenItems.empty()
                    ImGui::EndDisabled();  //! player

                    bSlotWarning = false;
                    for (auto i : params.items) {
                        if (auto armor = i->As<RE::TESObjectARMO>()) {
                            auto itemSlots =
                                MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
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

        if (popupSettings) ImGui::OpenPopup("Settings");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        bool bPopupActive = true;
        if (ImGui::BeginPopupModal("Settings", &bPopupActive, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("NOTE: Many changes require restarting Skyrim to take full effect.");
            if (ImGui::BeginTabBar("Settings Tabs")) {
                if (ImGui::BeginTabItem("General")) {
                    const char* logLevels[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};

                    ImGui::Text("Log verbsity");
                    ImGui::SameLine();
                    if (ImGui::BeginCombo("##Verbosity", logLevels[g_Config.verbosity],
                                          ImGuiComboFlags_PopupAlignLeft)) {
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

                    ImGui::Checkbox("Close console after qar command", &g_Config.bCloseConsole);
                    ImGui::Checkbox("Delete given items after applying changes", &g_Config.bAutoDeleteGiven);
                    ImGui::Checkbox("Round weights to 0.1", &g_Config.bRoundWeight);
                    ImGui::Checkbox("Reset sliders after changing mods", &g_Config.bResetSliders);
                    ImGui::Checkbox("Reset slot remapping after changing mods", &g_Config.bResetSlotRemap);
                    ImGui::Checkbox("Allow remapping armor slots to unhandled slots", &g_Config.bAllowInvalidRemap);
                    ImGui::Checkbox("Allow remapping of protected armor slots (Not recommended)",
                                    &g_Config.bEnableProtectedSlotRemapping);
                    MakeTooltip(
                        "Body, hands, feet, and head slots tend to break in various ways if you move them to other "
                        "slots.");
                    ImGui::Checkbox("Highlight things you may want to look at", &g_Config.bHighlights);
                    ImGui::Checkbox("Show Frostfall coverage slider even if not installed",
                                    &g_Config.bShowFrostfallCoverage);
                    ImGui::Checkbox("USE AT YOUR OWN RISK: Enable <All Items> in item list", &g_Config.bEnableAllItems);
                    MakeTooltip(
                        "This can result in performance issues, and making a mess by changing too many items at once.\n"
                        "Use with caution.");

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Distribution")) {
                    ImGui::Checkbox("Enforce loot rarity with empty loot", &g_Config.bEnableRarityNullLoot);
                    MakeTooltip(
                        "Example: The subset of items elible for loot has 1 rare item, 0 commons and uncommons\n"
                        "DISABLED: You will always get that rare item\n"
                        "ENABLED: You will have a 5%% chance at the item, and 95%% chance of nothing");

                    ImGui::Checkbox("Normalize drop rates between mods", &g_Config.bNormalizeModDrops);
                    ImGui::SliderFloat("Adjust drop rates", &g_Config.fDropRates, 0.0f, 300.0f, "%.0f%%",
                                       ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SliderInt("Drop curve level granularity", &g_Config.levelGranularity, 1, 5, "%d",
                                     ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip("A lower number generates more loot lists but more accurate distribution");

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Crafting")) {
                    ImGui::Text("Only generate crafting recipes for");
                    ImGui::SameLine();

                    if (ImGui::BeginCombo("##Rarity", rarity[g_Config.craftingRarityMax],
                                          ImGuiComboFlags_PopupAlignLeft)) {
                        for (int i = 0; rarity[i]; i++) {
                            bool selected = i == g_Config.craftingRarityMax;
                            if (ImGui::Selectable(rarity[i], selected)) g_Config.craftingRarityMax = i;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    ImGui::Text("rarity and below");

                    ImGui::Indent();
                    ImGui::Checkbox("Disable existing crafting recipies above that rarity",
                                    &g_Config.bDisableCraftingRecipesOnRarity);
                    ImGui::Unindent();

                    ImGui::Checkbox("Keep crafting books as requirement", &g_Config.bKeepCraftingBooks);

                    ImGui::Checkbox("Use recipes from as similar item if primary item is lacking",
                                    &g_Config.bUseSecondaryRecipes);

                    ImGui::Checkbox("Enable smelting recipes", &g_Config.bEnableSmeltingRecipes);

                    ImGui::Text("Instead of free recipes, make recipes cost gold as a portion of the item's value:");
                    ImGui::Indent();
                    ImGui::Text("Temper recipes");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##TemperRatio", &g_Config.fTemperGoldCostRatio, 0.0f, 200.0f, "%.0f%%",
                                       ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip("Set to 0%% to keep free");
                    ImGui::Text("Craft recipes");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##CraftRatio", &g_Config.fCraftGoldCostRatio, 0.0f, 200.0f, "%.0f%%",
                                       ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip("Set to 0%% to keep free");
                    ImGui::Unindent();

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Permissions")) {
                    if (ImGui::BeginTable("Permissions", 2,
                                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
                                              ImGuiTableColumnFlags_NoReorder)) {
                        ImGui::TableSetupColumn("Local Changes Permissions");
                        ImGui::TableSetupColumn("Shared Changes Permissions");

                        ImGui::TableHeadersRow();
                        ImGui::TableNextRow();

                        ImGui::TableNextColumn();
                        PermissionsChecklist("Local", g_Config.permLocal);

                        ImGui::TableNextColumn();
                        PermissionsChecklist("Shared", g_Config.permShared);

                        ImGui::EndTable();
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Hooks")) {
                    ImGui::Checkbox("Enable NIF armor slot remapping hook", &g_Config.bEnableArmorSlotModelFixHook);
                    MakeTooltip("Fixes armor pieces not rendering when remapping their armor slots");

                    ImGui::BeginDisabled(REL::Module::IsVR());
                    ImGui::Checkbox("Enable Skyrim warmth system hook", &g_Config.bEnableSkyrimWarmthHook);
                    MakeTooltip(
                        "Required to enable changes to item warmth.\n"
                        "WARNING: Likely to cause crashes on VR!");
                    ImGui::EndDisabled();

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::EndPopup();
        }
        if (!bPopupActive) g_Config.Save();

        if (popupRemapSlots) ImGui::OpenPopup("Remap Slots");

        ImGui::SetNextWindowSizeConstraints({300, 500}, {1600, 1000});
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;
        if (ImGui::BeginPopupModal("Remap Slots", &bPopupActive, ImGuiWindowFlags_NoScrollbar)) {
            static int nSlotView = 33;
            uint64_t slotsUsed = 0;
            ArmorSlots slotsProtected = g_Config.bEnableProtectedSlotRemapping ? 0 : kProtectedSlotMask;

            std::vector<RE::TESObjectARMO*> lsSlotItems;
            for (auto i : g_Config.acParams.items) {
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

            ImGui::Text("Click and drag from the original slot on the left to the new replacement slot on the right");
            ImGui::PushItemWidth(-FLT_MIN);

            if (ImGui::BeginTable("Slot Mapping", 3,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_PadOuterX | ImGuiTableFlags_PreciseWidths |
                                      ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Original");
                ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Remapped");

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
                            strWarn =
                                "Warning: Items in this slot will not be changed unless remapped to another slot.";
                            popCol++;
                        } else if ((1ull << i) & slotsUsed & (remappedTar & ~remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            strWarn =
                                "Warning: Other items are being remapped to this slot.\n"
                                "This will cause conflicts unless this slot is also remapped.";
                            popCol++;
                        }

                        bool bSelected = false;
                        if (ImGui::Selectable(strSlotDesc[i], &bSelected, 0)) {
                        }

                        if (strWarn) MakeTooltip(strWarn);

                        if (ImGui::BeginDragDropSource(0)) {
                            nSlotView = i;
                            g_Config.acParams.mapArmorSlots.erase(i);
                            ImGui::SetDragDropPayload("ARMOR SLOT", &i, sizeof(i));
                            ImGui::Text(strSlotDesc[i]);
                            ImGui::EndDragDropSource();
                        }

                        ImGui::SameLine();

                        float w = ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.x * 2;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                                             4.0f);  // Radio circles are weirdly offset down slightly?

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
                            "Warning: Items are being remapped to this slot, but other items are already using this "
                            "slot.");

                    if (!bProtected && (g_Config.bAllowInvalidRemap || !bDisabled) && ImGui::BeginDragDropTarget()) {
                        if (auto payload = ImGui::AcceptDragDropPayload("ARMOR SLOT")) {
                            g_Config.acParams.mapArmorSlots[*(int*)payload->Data] = i;
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(strSlotDesc[i], &bCheckbox)) {
                    }
                    tarCenter[i] = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) / 2;
                    tarCenter[i].x = ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().y -
                                     tarCenter[i].y;  // Want center of the circle
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

                for (auto i : g_Config.acParams.mapArmorSlots) {
                    draw->AddLine(srcCenter[i.first], tarCenter[i.second], colorChanged, lineWidth);
                }
            }

            ImGui::EndPopup();
        }

        static WordSet wordsUsed;
        static WordSet wordsStatic;
        static std::map<const DynamicVariant*, std::vector<std::size_t>> mapDVWords;

        if (popupDynamicVariants) {
            ImGui::OpenPopup("Dynamic Variants");

            nShowWords = AnalyzeResults::eWords_StaticVariants;
            wordsStatic.clear();
            wordsUsed.clear();
            mapDVWords.clear();

            auto& ws = analyzeResults.sets[AnalyzeResults::eWords_StaticVariants];
            wordsStatic.insert(ws.begin(), ws.end());

            wordsUsed.insert(wordsStatic.begin(), wordsStatic.end());
            for (auto& i : mapDVWords) {
                wordsUsed.insert(i.second.begin(), i.second.end());
            }
        }

        ImGui::SetNextWindowSizeConstraints({300, 500}, {1600, 1000});
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;

        static bool dvWndWasOpen = false;
        if (ImGui::BeginPopupModal("Dynamic Variants", &bPopupActive, ImGuiWindowFlags_NoScrollbar)) {
            dvWndWasOpen = true;
            ImGui::Text("Drag the appropriate words (if any) to their associated dynamic type on the right side.");

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

            if (ImGui::BeginTable(
                    "WindowTable", 3,
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableSetupColumn("Static Variants");
                ImGui::TableSetupColumn("Unassigned");
                ImGui::TableSetupColumn("Dynamic Variants");

                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableNextColumn();
                ImGui::TableHeader("Static Variants");
                MakeTooltip(
                    "Static Variants are similar but distinct items.\n"
                    "Usually colors or different versions of an item piece.\n\n"
                    "The accurracy of words in this table is not important.");

                ImGui::TableNextColumn();
                ImGui::TableHeader("Unassigned");

                ImGui::TableNextColumn();
                ImGui::TableHeader("Dynamic Variants");
                MakeTooltip(
                    "Dynamic Variants are the same item in different forms.\n\n"
                    "Place associated words under the associated variant type.\n"
                    "If there are multiple, they should be ordered that the most default is at the top and the most "
                    "changed at the bottom.");

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
                    if (ImGui::Button("Show More Words")) {
                        nShowWords = std::min(nShowWords + 1, AnalyzeResults::eWords_Count - 1);
                    }

                    if (ImGui::Button("Show Less Words")) {
                        nShowWords = std::max(nShowWords - 1, (int)AnalyzeResults::eWords_StaticVariants);
                    }
                    ImGui::PopItemWidth();
                    ImGui::EndListBox();

                    DragDropWords::Target(nullptr);
                }

                ImGui::TableNextColumn();
                for (const auto& dv : g_Config.mapDynamicVariants) {
                    if (ImGui::CollapsingHeader(dv.first.c_str(),
                                                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet)) {
                        DragDropWords::Target(&mapDVWords[&dv.second], 0);

                        auto copy = mapDVWords[&dv.second]; //Drag drop can modify 
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

                ImGui::EndTable();
            }

            ImGui::EndPopup();
        } else {
            if (dvWndWasOpen) {
                g_Config.acParams.dvSets = MapVariants(analyzeResults, mapDVWords);
            }
        }
    }

    ImGuiIntegration::BlockInput(!ImGui::IsWindowCollapsed(), ImGui::IsItemHovered());
    ImGui::End();

    if (!isActive) ImGuiIntegration::Show(false);
}