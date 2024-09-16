#include "WarmthHook.h"

#include "Data.h"

// Ability to hook entirely thanks to Survival Control Panel
// https://github.com/colinswrath/SurvivalControlPanel

namespace {
    using namespace QuickArmorRebalance;

    struct ArmorWarmthInfo {
        void* _pad_0;
        std::int32_t* slotMask;
        RE::Setting*** warmthValues;
    };
    static_assert(sizeof(ArmorWarmthInfo) == 0x18);

    struct FakeGameSetting {
        FakeGameSetting(float val = 0.0f) : data(val) {}

        std::int64_t _pad_0 = 0;
        float data;
        char* name = nullptr;
    };
    static_assert(sizeof(FakeGameSetting) == sizeof(RE::Setting));

    static void GetWarmthRating(RE::BGSKeywordForm* form, ArmorWarmthInfo* info) {
        static FakeGameSetting fake;
        static FakeGameSetting* fakePtr(&fake);

        if (auto armor = skyrim_cast<RE::TESObjectARMO*>(form)) {
            auto it = g_Data.modifiedWarmth.find(armor);
            if (it != g_Data.modifiedWarmth.end()) {
                fake.data = it->second;

                *info->slotMask = 1;
                *info->warmthValues = reinterpret_cast<RE::Setting**>(&fakePtr);
            }
        }
    }

    struct GetWarmthRatingHook {
        static constexpr auto id = REL::VariantID(25833, 26393, 0);
        static constexpr auto offset = REL::VariantOffset(0x6E, 0x6C, 0);

        static void thunk(RE::BGSKeywordForm* form, ArmorWarmthInfo* info) {
            func(form, info);
            GetWarmthRating(form, info);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct WarmthCalcFuncVisitHook {
        static constexpr auto id = REL::ID(26404);  // Might be @0x3BDD30 in SE?
        static constexpr auto offset = REL::Offset(0x78);

        static void thunk(RE::BGSKeywordForm* form, ArmorWarmthInfo* info) {
            func(form, info);
            GetWarmthRating(form, info);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
}

void QuickArmorRebalance::InstallWarmthHooks() {
    SKSE::AllocTrampoline(14 * 1);
    write_thunk_call<GetWarmthRatingHook>();

    if (REL::Module::IsAE()) {
        SKSE::AllocTrampoline(14 * 1);
        write_thunk_call<WarmthCalcFuncVisitHook>();
    }
}
