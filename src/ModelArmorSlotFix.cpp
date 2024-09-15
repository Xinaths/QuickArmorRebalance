#include "ModelArmorSlotFix.h"


struct ProcessGeometryHook {
    static constexpr auto id = REL::VariantID(15535, 15712, 0);
    static constexpr auto offset = REL::VariantOffset(0x79A, 0x72F, 0);

    static void thunk(RE::BipedAnim* a_biped, RE::BSGeometry* a_object,
                      RE::BSDismemberSkinInstance* a_dismemberInstance, std::int32_t a_slot, bool a_unk05) {
        func(a_biped, a_object, a_dismemberInstance, a_slot, a_unk05);

        a_object->CullNode(false);

        /*
        if (auto userData = a_object->GetUserData()) {
            logger::info("name: {} [{}]", userData->GetName(), a_slot);
            if (auto armor = userData->As<RE::TESObjectARMO>()) {
                logger::info("Process geometry: {}", armor->GetName());
            }
        }
        */
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

struct HideShowBufferedSkinHook {
    static constexpr auto id = REL::VariantID(15501, 15678, 0);
    static constexpr auto offset = REL::Offset(0x1EA);

    static void thunk(RE::BipedAnim* a_biped, RE::NiAVObject* a_object, std::int32_t a_slot, bool a_unk04) {
        func(a_biped, a_object, a_slot, a_unk04);

        for (int i = 0; i < 32; i++) {
            if (auto item = a_biped->objects[i].item) {
                logger::info("[{}] has {}", i, item->GetName());
            }
        }
        /*
        if (auto userData = a_object->GetUserData()) {
            logger::info("name: {} [{}]", userData->GetName(), a_slot);
            if (auto armor = userData->As<RE::TESObjectARMO>()) {
                logger::info("Process geometry: {}", armor->GetName());
            }
        }
        */
    }
    static inline REL::Relocation<decltype(thunk)> func;
};


void QuickArmorRebalance::InstallModelArmorSlotFixHooks() {
    return;

    SKSE::AllocTrampoline(14 * 2);

    write_thunk_call<ProcessGeometryHook>();
    write_thunk_call<HideShowBufferedSkinHook>();
}
