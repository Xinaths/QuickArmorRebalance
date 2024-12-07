#include "ModelArmorSlotFix.h"

#include "Data.h"

/*////////////////////////////////////////////////////////////////////
    When changing armor slots, there's 3 places that need to be changed
    - The armor itself
    - The armor addon
    - The nif (model) file

    If these don't match you get invisible armor parts

    This code sits in the middle of the loading code for the part of nifs that loads body slots
    If the filenames match, it changes non-matching slots to be ones that match

    There is almost certainly a better way to do this, but this is lightweight enough
    that I don't feel the need to go digging deep into Skyrims code to find it
*//////////////////////////////////////////////////////////////////////


namespace {
    using namespace QuickArmorRebalance;

    struct BSDismemberSkinInstance_LoadBinary_Hook {
        static constexpr auto id = RE::VTABLE_BSDismemberSkinInstance[0];
        static constexpr auto offset = REL::Offset(0x18);

        static void thunk(RE::BSDismemberSkinInstance* skin, RE::NiStream& a_stream) {
            func(skin, a_stream);

            constexpr std::string_view strPrefix{"data\\MESHES\\"};
            if (!strncmp(a_stream.inputFilePath, strPrefix.data(), strPrefix.length())) {
                std::string modelPath(a_stream.inputFilePath + strPrefix.length());
                ToLower(modelPath);

                auto hash = std::hash<std::string>{}(modelPath);
                auto it = g_Data.remapFileArmorSlots.find(hash);
                if (it != g_Data.remapFileArmorSlots.end()) {
                    auto slots = it->second;
                    auto missing = slots;

                    auto& data = skin->GetRuntimeData();


                    //First pass - figure out what slots are missing
                    for (int i = 0; i < data.numPartitions; i++) {
                        auto& part = data.partitions[i];
                        if (part.slot < 30 || part.slot > 62) continue;
                        missing &= ~(1 << (part.slot - 30));
                        if (!missing) return; //Have everything, can abort
                    }

                    //Second pass - update slots for missing parts
                    for (int i = 0; i < data.numPartitions; i++) {
                        auto& part = data.partitions[i];
                        if (part.slot < 30 || part.slot > 62) continue;
                        if (((1 << (part.slot - 30)) & slots) == 0) { //Mismatched slot - remap
                            unsigned long slot;
                            _BitScanForward(&slot, missing);
                            missing &= missing - 1;  // Removes lowest bit

                            //logger::info("{} changing {}->{}", a_stream.inputFilePath, part.slot, slot + 30);
                            part.slot = (uint16_t)slot + 30;
                            if (!missing) break;
                        }
                    }
                }
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

}

void QuickArmorRebalance::InstallModelArmorSlotFixHooks() {
    HookVirtualFunction<BSDismemberSkinInstance_LoadBinary_Hook>();
}
