#include "UI.h"

#include "ArmorChanger.h"
#include "ArmorSetBuilder.h"
#include "Config.h"
#include "Data.h"
#include "ImGuiIntegration.h"

using namespace QuickArmorRebalance;

bool StringContainsI(const char* s1, const char* s2) {
    std::string str1(s1), str2(s2);
    std::transform(str1.begin(), str1.end(), str1.begin(), ::tolower);
    std::transform(str2.begin(), str2.end(), str2.begin(), ::tolower);
    return str1.contains(str2);
}

void MakeTooltip(const char* str) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(str);
}

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

    void Give(RE::TESBoundObject* item, bool equip = false) {
        if (!item) return;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        player->AddObjectToContainer(item, nullptr, 1, nullptr);
        items.push_back(item);

        // After much testing, Skyrim seems to REALLY not like equipping more then one item per
        // frame And the per frame limit counts while the console is open. Doing so results in it
        // letting you equip as many items in one slot as you'd like So instead, have to mimic
        // the expected behavior and hopefully its good enough

        if (equip) {
            if (auto armor = item->As<RE::TESObjectARMO>()) {
                auto slots = (unsigned int)armor->GetSlotMask();
                if ((slots & recentEquipSlots) == 0) {
                    recentEquipSlots |= slots;

                    // Not using AddTask will result in it not un-equipping current items
                    SKSE::GetTaskInterface()->AddTask([=]() {
                        RE::ActorEquipManager::GetSingleton()->EquipObject(player, item, nullptr, 1,
                                                                           armor->GetEquipSlot(), false, false, false);
                    });
                }
            }
        }
    }

    void Remove() {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        for (auto i : items) player->RemoveItem(i, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);

        items.clear();
    }

    unsigned int recentEquipSlots = 0;
    std::vector<RE::TESBoundObject*> items;
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

void SliderRow(const char* field, ArmorChangeParams::SliderPair& pair, float min = 0.0f, float max = 300.0f) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(field);
    ImGui::Checkbox(std::format("Modify {}", field).c_str(), &pair.bModify);
    ImGui::BeginDisabled(!pair.bModify);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::SliderFloat("##Scale", &pair.fScale, min, max, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TableNextColumn();
    if (ImGui::Button("Reset")) pair.fScale = 100.0f;
    ImGui::EndDisabled();
    ImGui::PopID();
}

void PermissionsChecklist(const char* id, Permissions& p) {
    ImGui::PushID(id);
    ImGui::Checkbox("Distribute loot", &p.bDistributeLoot);
    ImGui::Checkbox("Modify keywords", &p.bModifyKeywords);
    ImGui::Checkbox("Modify armor rating", &p.bModifyArmorRating);
    ImGui::Checkbox("Modify armor weight", &p.bModifyWeight);
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

void GetCurrentListItems(ModData* curMod, bool bFilterModified, const char* itemFilter) {
    ArmorChangeParams& params = g_Config.acParams;
    params.items.clear();
    if (curMod) {
        for (auto i : curMod->items) {
            if (bFilterModified && (g_Data.modifiedItems.contains(i) || g_Data.modifiedItemsShared.contains(i)))
                continue;
            if (*itemFilter && !StringContainsI(i->GetName(), itemFilter)) continue;

            params.items.push_back(i);
        }
    } else {
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            for (auto& item : player->GetInventory()) {
                if (item.second.second->IsWorn() && item.first->IsArmor()) {
                    if (auto i = item.first->As<RE::TESObjectARMO>()) {
                        if (!IsValidItem(i)) continue;
                        if (bFilterModified &&
                            (g_Data.modifiedItems.contains(i) || g_Data.modifiedItemsShared.contains(i)))
                            continue;
                        if (*itemFilter && !StringContainsI(i->GetFullName(), itemFilter)) continue;

                        params.items.push_back(i);
                    }
                }
            }
        }
    }

    if (!params.items.empty()) {
        std::sort(params.items.begin(), params.items.end(),
                  [](RE::TESBoundObject* const a, RE::TESBoundObject* const b) {
                      return _stricmp(a->GetName(), b->GetName()) < 0;
                  });
    }
}

bool WillBeModified(RE::TESBoundObject* i) {
    if (auto armor = i->As<RE::TESObjectARMO>()) {
        if ((g_Config.slotsWillChange & (ArmorSlots)armor->GetSlotMask()) == 0) return false;
    } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
        if (!g_Config.acParams.armorSet->FindMatching(weap)) return false;
    } else if (auto ammo = i->As<RE::TESAmmo>()) {
        if (!g_Config.acParams.armorSet->FindMatching(ammo)) return false;
    } else
        return false;

    return true;
}

short g_highlightRound = 0;

struct HighlightTrack {
    short round = -1;
    char pop = 0;
    bool enabled = false;

    void Touch() { round = g_highlightRound; }
    operator bool() { return !enabled || g_highlightRound == round; }

    void Push(bool show = true) {
        enabled = show;
        if (g_Config.bHighlights && round != g_highlightRound && show) {
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
        if (ImGui::IsItemActive()) round = g_highlightRound;

        ImGui::PopStyleVar(pop);
        ImGui::PopStyleColor(pop);
    }
};

void QuickArmorRebalance::RenderUI() {
    const auto colorChanged = IM_COL32(0, 255, 0, 255);
    const auto colorChangedShared = IM_COL32(255, 255, 0, 255);
    const auto colorDeleted = IM_COL32(255, 0, 0, 255);

    const char* rarity[] = {"Common", "Uncommon", "Rare", nullptr};

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

    static HighlightTrack hlConvert;
    static HighlightTrack hlDistributeAs;
    static HighlightTrack hlRarity;

    auto isInventoryOpen = RE::UI::GetSingleton()->IsItemMenuOpen();

    static bool bMenuHovered = false;
    bool popupSettings = false;

    ImGuiWindowFlags wndFlags = ImGuiWindowFlags_NoScrollbar;
    if (bMenuHovered) wndFlags |= ImGuiWindowFlags_MenuBar;

    ImGui::SetNextWindowSizeConstraints({700, 250}, {1600, 1000});
    if (ImGui::Begin("Quick Armor Rebalance", &isActive, wndFlags)) {
        if (g_Config.strCriticalError.empty()) {
            ArmorChangeParams& params = g_Config.acParams;

            const char* noMod = "<Currently Worn Armor>";
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

            ImGui::PushItemWidth(-FLT_MIN);

            if (ImGui::BeginTable("WindowTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("LeftCol", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("RightCol", ImGuiTableColumnFlags_WidthFixed,
                                        0.25f * ImGui::GetContentRegionAvail().x);

                static bool bFilterModified = false;
                static char itemFilter[200] = "";
                bool showPopup = false;

                ImGui::TableNextColumn();
                if (ImGui::BeginChild("LeftPane")) {
                    ImGui::PushItemWidth(-FLT_MIN);

                    if (ImGui::BeginTable("Mod Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("ModCol1");
                        ImGui::TableSetupColumn("ModCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ModCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text("Mod");
                        ImGui::TableNextColumn();

                        RE::TESFile* blacklist = nullptr;

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::BeginCombo("##Mod", curMod ? curMod->mod->fileName : noMod,
                                              ImGuiComboFlags_HeightLarge)) {
                            {
                                bool selected = !curMod;
                                if (ImGui::Selectable(noMod, selected)) curMod = nullptr;
                                if (selected) ImGui::SetItemDefaultFocus();
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
                                        params.weapon.damage.fScale = 100.0f;
                                        params.weapon.speed.fScale = 100.0f;
                                        params.weapon.weight.fScale = 100.0f;
                                        params.weapon.stagger.fScale = 100.0f;
                                        params.value.fScale = 100.0f;
                                    }

                                    g_highlightRound++;
                                }
                                if (isCtrlDown && isAltDown && ImGui::IsItemHovered()  && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
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

                    ImGui::Text("Filter Items");
                    ImGui::Indent();
                    ImGui::Text("Name contains");
                    ImGui::SameLine();
                    if (ImGui::InputText("##ItemFilter", itemFilter, sizeof(itemFilter) - 1)) g_highlightRound++;
                    if (ImGui::Checkbox("Hide modified items", &bFilterModified)) g_highlightRound++;
                    ImGui::Unindent();

                    if (ImGui::BeginTable("Convert Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("ConvertCol1");
                        ImGui::TableSetupColumn("ConvertCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ConvertCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text("Convert to");
                        ImGui::TableNextColumn();

                        if (!params.armorSet) params.armorSet = &g_Config.armorSets[0];

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

                        hlConvert.Push();
                        if (ImGui::BeginCombo("##ConvertTo", params.armorSet->name.c_str(),
                                              ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                            for (auto& i : g_Config.armorSets) {
                                bool selected = params.armorSet == &i;
                                ImGui::PushID(i.name.c_str());
                                if (ImGui::Selectable(i.name.c_str(), selected)) params.armorSet = &i;
                                if (selected) ImGui::SetItemDefaultFocus();
                                ImGui::PopID();
                            }

                            ImGui::EndCombo();
                        }
                        hlConvert.Pop();

                        ImGui::TableNextColumn();

                        static HighlightTrack hlApply;
                        hlApply.Push(hlConvert && hlDistributeAs && hlRarity);

                        if (ImGui::Button("Apply changes")) {
                            g_Config.Save();
                            MakeArmorChanges(params);

                            if (g_Config.bAutoDeleteGiven) givenItems.Remove();

                            hlConvert.Touch();
                            hlDistributeAs.Touch();
                            hlRarity.Touch();
                        }

                        hlApply.Pop();

                        ImGui::EndTable();
                    }

                    GetCurrentListItems(curMod, bFilterModified, itemFilter);
                    g_Config.slotsWillChange = GetConvertableArmorSlots(params);

                    bool hasEnabledArmor = false;
                    bool hasEnabledWeap = false;

                    for (auto i : params.items) {
                        if (i->As<RE::TESObjectARMO>()) {
                            if (!uncheckedItems.contains(i)) hasEnabledArmor = true;
                        } else if (i->As<RE::TESObjectWEAP>()) {
                            if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
                        } else if (i->As<RE::TESAmmo>()) {
                            if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
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
                    // ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
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
                    ImGui::Unindent(60);

                    ImGui::EndDisabled();
                    ImGui::EndDisabled();

                    // Modifications
                    // ImGui::SetNextItemSize();

                    // ImGui::PushStyleColor(ImGuiCol_ChildBg, colorDeleted);
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
                            //if (iTabOpen != iTabSelected) ImGui::SetTabItemClosed(tabLabels[iTabSelected]);

                            iTabSelected = iTabOpen;

                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(tabLabels[iTab], nullptr,
                                                    bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow("Armor Rating", params.armor.rating);
                                    SliderRow("Weight", params.armor.weight);

                                    ImGui::EndTable();
                                }

                                ImGui::Separator();
                                ImGui::Text("Stat distribution curve");
                                ImGui::SameLine();

                                static auto* curCurve = &g_Config.curves[0];

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
                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(tabLabels[iTab], nullptr,
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
                    // ImGui::PopStyleColor();

                    // ImGui::Separator();

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
                        ImGui::Checkbox("Make free if no recipe to copy##Temper", &params.temper.bFree);
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
                        ImGui::Checkbox("Make free if no recipe to copy##Craft", &params.craft.bFree);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "If the item being copied lacks a crafting recipe, checking will remove all\n"
                                "components and conditions to craft the modified item");
                        ImGui::EndDisabled();

                        ImGui::EndTable();
                    }

                    ImGui::PopItemWidth();
                }
                ImGui::EndChild();

                ImGui::TableNextColumn();
                ImGui::Text("Items");

                // ImGui::SameLine();
                // RightButton("Settings");

                auto avail = ImGui::GetContentRegionAvail();
                avail.y -= ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.y * 2;

                auto player = RE::PlayerCharacter::GetSingleton();
                decltype(params.items) finalItems;
                finalItems.reserve(params.items.size());

                bool modChangesDeleted = g_Data.modifiedFilesDeleted.contains(curMod->mod);

                if (ImGui::BeginListBox("##Items", avail)) {
                    if (ImGui::BeginPopupContextWindow()) {
                        if (ImGui::Selectable("Enable all")) uncheckedItems.clear();
                        if (ImGui::Selectable("Disable all"))
                            for (auto i : params.items) uncheckedItems.insert(i);
                        ImGui::Separator();

                        ImGui::BeginDisabled(selectedItems.empty());
                        if (ImGui::Selectable("Enable selected")) {
                            for (auto i : selectedItems) uncheckedItems.erase(i);
                        }
                        if (ImGui::Selectable("Disable selected")) {
                            for (auto i : selectedItems) uncheckedItems.insert(i);
                        }
                        ImGui::EndDisabled();

                        ImGui::EndPopup();
                    }

                    if (!selectedItems.contains(lastSelectedItem)) lastSelectedItem = nullptr;

                    // Clear out selections that are no longer visible
                    auto lastSelected(std::move(selectedItems));
                    for (auto i : params.items)
                        if (lastSelected.contains(i)) selectedItems.insert(i);

                    for (auto i : params.items) {
                        int pop = 0;
                        if (g_Data.modifiedItems.contains(i)) {
                            if (modChangesDeleted)
                                ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            else
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                            pop++;
                        } else if (g_Data.modifiedItemsShared.contains(i)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                            pop++;
                        } else if (!WillBeModified(i)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                            pop++;
                        }

                        std::string name(i->GetName());
                        // if (!i->fullName.empty()) name = i->GetFullName();

                        if (name.empty()) name = std::format("{}:{:010x}", i->GetFile(0)->fileName, i->formID);

                        ImGui::BeginGroup();

                        bool isChecked = !uncheckedItems.contains(i);
                        ImGui::PushID(name.c_str());
                        if (ImGui::Checkbox("##ItemCheckbox", &isChecked)) {
                            if (!isChecked)
                                uncheckedItems.insert(i);
                            else
                                uncheckedItems.erase(i);
                        }
                        if (isChecked) finalItems.push_back(i);

                        // ImGui::PopID();

                        ImGui::SameLine();

                        bool selected = selectedItems.contains(i);

                        // ImGui::PushID(name.c_str());
                        if (ImGui::Selectable(name.c_str(), &selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                if (!isInventoryOpen) {
                                    if (isShiftDown) {
                                        if (isAltDown) {
                                            auto armorSet = BuildSetFrom(i, params.items);

                                            if (!isCtrlDown) selectedItems.clear();
                                            for (auto piece : armorSet) selectedItems.insert(piece);
                                        }
                                    } else {
                                        if (isAltDown) {
                                            auto armorSet = BuildSetFrom(i, params.items);
                                            givenItems.UnequipCurrent();
                                            for (auto piece : armorSet) givenItems.Give(piece, true);
                                        } else {
                                            givenItems.Give(i, true);
                                        }
                                    }
                                }
                            } else {
                                if (!isCtrlDown) selectedItems.clear();

                                if (!isShiftDown) {
                                    if (selected)
                                        selectedItems.insert(i);
                                    else
                                        selectedItems.erase(i);

                                    lastSelectedItem = i;
                                } else {
                                    if (lastSelectedItem) {
                                        bool adding = false;
                                        for (auto j : params.items) {
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

                        ImGui::PopID();
                        ImGui::EndGroup();
                        ImGui::PopStyleColor(pop);
                    }
                    ImGui::EndListBox();
                }

                ImGui::BeginDisabled(!player || !curMod || isInventoryOpen);

                if (ImGui::Button(selectedItems.empty() ? "Give All" : "Give Selected")) {
                    if (selectedItems.empty())
                        for (auto i : params.items) {
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
                        for (auto i : params.items) {
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

                params.items = std::move(finalItems);

                ImGui::EndTable();
            }

            ImGui::PopItemWidth();
        } else {
            ImGui::Text(g_Config.strCriticalError.c_str());
        }

        if (popupSettings) ImGui::OpenPopup("Settings");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        bool bPopupActive = true;
        if (ImGui::BeginPopupModal("Settings", &bPopupActive, ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* logLevels[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};

            ImGui::Text("Log verbsity");
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

            ImGui::Checkbox("Close console after qar command", &g_Config.bCloseConsole);
            ImGui::Checkbox("Delete given items after applying changes", &g_Config.bAutoDeleteGiven);
            ImGui::Checkbox("Round weights to 0.1", &g_Config.bRoundWeight);
            ImGui::Checkbox("Reset sliders after changing mods", &g_Config.bResetSliders);
            ImGui::Checkbox("Highlight things you may want to look at", &g_Config.bHighlights);

            ImGui::Separator();
            ImGui::Checkbox("Enforce loot rarity with empty loot", &g_Config.bEnableRarityNullLoot);
            MakeTooltip("Example: The subset of items elible for loot has 1 rare item, 0 commons and uncommons\n"
                "DISABLED: You will always get that rare item\n"
                "ENABLED: You will have a 5%% chance at the item, and 95%% chance of nothing");

            ImGui::Checkbox("Normalize drop rates between mods", &g_Config.bNormalizeModDrops);
            ImGui::SliderFloat("Adjust drop rates", &g_Config.fDropRates, 0.0f, 300.0f, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderInt("Drop curve level granularity", &g_Config.levelGranularity, 1, 5, "%d",
                             ImGuiSliderFlags_AlwaysClamp);
            MakeTooltip("A lower number generates more loot lists but more accurate distribution");

            ImGui::Separator();
            ImGui::Text("Only generate crafting recipes for");
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
            ImGui::Text("rarity and below");

            ImGui::Indent();
            ImGui::Checkbox("Disable existing crafting recipies above that rarity",
                            &g_Config.bDisableCraftingRecipesOnRarity);
            ImGui::Unindent();

            ImGui::Checkbox("Keep crafting books as requirement", &g_Config.bKeepCraftingBooks);

            if (ImGui::BeginTable(
                    "Permissions", 2,
                    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableColumnFlags_NoReorder)) {
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

            ImGui::EndPopup();
        }
        if (!bPopupActive) g_Config.Save();
    }

    ImGuiIntegration::BlockInput(!ImGui::IsWindowCollapsed(), ImGui::IsItemHovered());
    ImGui::End();

    if (!isActive) ImGuiIntegration::Show(false);
}