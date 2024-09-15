#pragma once

#ifdef _MSC_VER
    #define _CRT_SECURE_NO_WARNINGS
#endif

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include "rapidjson/document.h"

#define TOML_EXCEPTIONS 0
#include "toml++/toml.hpp"

#include <imgui.h>

using namespace std::literals;

#define PLUGIN_NAME "QuickArmorRebalance"

#include "logger.h"


inline RE::FormID GetFullId(const RE::TESFile* file, RE::FormID id) {
    return ((RE::FormID)file->compileIndex << 24) | (file->smallFileCompileIndex << 12) | id;
}

inline RE::FormID GetFileId(const RE::TESForm* f) {
    auto id = f->GetLocalFormID();
    return f->GetFile(0)->IsLight() ? id & 0xfff : id & 0xffffff;
}

namespace QuickArmorRebalance {
    template <class T>
    void write_thunk_call() {
        auto& trampoline = SKSE::GetTrampoline();
        REL::Relocation<std::uintptr_t> hook{T::id, T::offset};
        T::func = trampoline.write_call<5>(hook.address(), T::thunk);
    }
}
