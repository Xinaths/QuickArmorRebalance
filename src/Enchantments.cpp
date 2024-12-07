#include "Enchantments.h"
#include "Data.h"


namespace {
    using namespace QuickArmorRebalance;

    struct TESObjectREFR_Initialize {
        //Hook found from Container Distribution Framework
        // https://www.nexusmods.com/skyrimspecialedition/mods/120152
        // https://github.com/SeaSparrowOG/DynamicContainerInventoryFramework

        static constexpr auto id = REL::ID(19507);
        static constexpr auto offset = REL::Offset(0x78C);

        static void thunk(RE::TESObjectREFR* a_this, bool a3) {
            func(a_this, a3);

            if (!a_this->GetBaseObject()) return;
            auto cont = a_this->GetBaseObject()->As<RE::TESObjectCONT>();
            if (!cont) return;

            if (!g_Data.distContainers.contains(cont)) return;

            logger::info("Initializing: {} ", a_this->GetName());

            auto form = RE::TESForm::LookupByID(0x00049509);
            if (!form) return;

            auto fortHealth = form->As<RE::EnchantmentItem>();
            if (!fortHealth) return;

            for (auto& item : a_this->GetInventory()) {
                if (g_Data.distItems.contains(item.first)) {
                    logger::info("- Has: {} x{}", item.first->GetName(), item.second.first);

                    auto entry = item.second.second.get();
                    if (entry->extraLists) {
                        for (auto& ls : *entry->extraLists) {
                            ls->Add(new RE::ExtraEnchantment(fortHealth, 0));

                            logger::info("-- Extra list start");
                            for (auto& extra : *ls) {
                                if (extra.GetType() == RE::ExtraDataType::kLeveledItem) {
                                    auto p = static_cast<RE::ExtraLeveledItem*>(&extra);
                                    logger::info("---- Extra: {:#10x}", p->levItem);
                                }
                            }                            
                        }
                    }
                }
            }

        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

}

void QuickArmorRebalance::InstallEnchantmentHooks() { 
    SKSE::AllocTrampoline(14 * 1);
    write_thunk_call<TESObjectREFR_Initialize>();
     
 }
