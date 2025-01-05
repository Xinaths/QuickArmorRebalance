// Largely copied / adapted from the code of CatHub, thanks pentalimbed!
//   https://www.nexusmods.com/skyrimspecialedition/mods/65958
//   https://github.com/Pentalimbed/cathub/tree/cathub-ng

#include "ImGUIIntegration.h"

#include <dinput.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "SKSE/SKSE.h"

namespace logger = SKSE::log;

static void (*g_RenderCallback)() = nullptr;
static void (*g_LoadFontCallback)() = nullptr;
bool g_showImGui = false;
bool g_blockInput = true;
bool g_blockClicks = false;

template <class T>
void write_thunk_call() {
    auto& trampoline = SKSE::GetTrampoline();
    REL::Relocation<std::uintptr_t> hook{T::id, T::offset};
    T::func = trampoline.write_call<5>(hook.address(), T::thunk);
}

class InputListener : public RE::BSTEventSink<RE::InputEvent*> {
public:
    static InputListener* GetSingleton() {
        static InputListener listener;
        return std::addressof(listener);
    }

    virtual RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;
};

// ImGui says to paste the below line
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct WndProcHook {
    static LRESULT thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (g_showImGui) {
            // if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) return true;

            auto& io = ImGui::GetIO();
            if (uMsg == WM_KILLFOCUS) {
                io.ClearInputCharacters();
                io.ClearInputKeys();
            }
        }

        return func(hWnd, uMsg, wParam, lParam);
    }
    static inline WNDPROC func;
};

struct D3DInitHook {
    static constexpr auto id = REL::VariantID(75595, 77226, 0xDC5530);
    static constexpr auto offset = REL::VariantOffset(0x9, 0x275, 0x9);

    static void thunk() {
        func();

        logger::debug("D3DInit Hooked!");

        /*
        auto render_manager = RE::BSGraphics::Renderer::GetSingleton();
        if (!render_manager) {
            logger::error("Cannot find render manager. Initialization failed!");
            return;
        }
        auto render_data = render_manager->GetRuntimeData();

        logger::debug("Getting swapchain...");
        auto swapchain = render_data.swapChain;
        if (!swapchain) {
            logger::error("Cannot find swapchain. Initialization failed!");
            return;
        }
        */

        IDXGISwapChain* swapchain = nullptr;

        
        auto& render_data = RE::BSRenderManager::GetSingleton()->GetRuntimeData(); //RE::BSGraphics::Renderer::GetSingleton()->data;
        auto device = render_data.forwarder;
        auto context = render_data.context;

        swapchain = render_data.swapChain;

        /*
        logger::debug("Getting swapchain");
        if (REL::Module::GetRuntime() != REL::Module::Runtime::VR) {
            auto renderWindow = *REL::Relocation<RE::BSGraphics::RendererWindow**>{
                RELOCATION_ID(524730, 411349)};  // From RE::BSGraphics::Renderer::GetCurrentWindow, but not in the current release
            swapchain = renderWindow->swapChain;
        } else {
            swapchain = render_data.renderWindows[0].swapChain;
        }
        */

        logger::debug("Getting swapchain desc...");
        DXGI_SWAP_CHAIN_DESC sd{};
        if (swapchain->GetDesc(std::addressof(sd)) < 0) {
            logger::error("IDXGISwapChain::GetDesc failed.");
            return;
        }

        logger::debug("Initializing ImGui...");
        ImGui::CreateContext();

        logger::debug("ImGui Win32 Init");
        if (!ImGui_ImplWin32_Init(sd.OutputWindow)) {
            logger::error("ImGui initialization failed (Win32)");
            return;
        }

        logger::debug("ImGui DX11 Init");
        if (!ImGui_ImplDX11_Init(device, context)) {
            logger::error("ImGui initialization failed (DX11)");
            return;
        }
        logger::debug("ImGui initialized!");

        // initialized.store(true);

        WndProcHook::func = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(sd.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));
        if (!WndProcHook::func) logger::error("SetWindowLongPtrA failed!");
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

struct DXGIPresentHook {
    static constexpr auto id = REL::VariantID(75461, 77246, 0xDBBDD0);
    static constexpr auto offset = REL::VariantOffset(0x9, 0x9, 0x15);

    static void thunk(std::uint32_t a_p1) {
        func(a_p1);

        if (g_LoadFontCallback) {
            g_LoadFontCallback();
            g_LoadFontCallback = nullptr;
        }

        ImGui_ImplWin32_NewFrame();  // Let imgui clear out any queued messages and whatnot

        // Its best to skip the stuff below if possible, but there's a situation where input messages are queued and not processed until frames are rendered
        // This forces it to update once in a while to clear out anything queued
        if (!g_showImGui) {
            ImGui::GetIO().SetAppAcceptingEvents(false);
            return;
        }
        ImGui::GetIO().SetAppAcceptingEvents(true);

        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();

        if (g_showImGui && g_RenderCallback) g_RenderCallback();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

////////////////////////////////////////////////////////////////
// Input handling

#define IM_VK_KEYPAD_ENTER (VK_RETURN + 256)
static ImGuiKey ImGui_ImplWin32_VirtualKeyToImGuiKey(WPARAM wParam) {
    switch (wParam) {
        case VK_TAB:
            return ImGuiKey_Tab;
        case VK_LEFT:
            return ImGuiKey_LeftArrow;
        case VK_RIGHT:
            return ImGuiKey_RightArrow;
        case VK_UP:
            return ImGuiKey_UpArrow;
        case VK_DOWN:
            return ImGuiKey_DownArrow;
        case VK_PRIOR:
            return ImGuiKey_PageUp;
        case VK_NEXT:
            return ImGuiKey_PageDown;
        case VK_HOME:
            return ImGuiKey_Home;
        case VK_END:
            return ImGuiKey_End;
        case VK_INSERT:
            return ImGuiKey_Insert;
        case VK_DELETE:
            return ImGuiKey_Delete;
        case VK_BACK:
            return ImGuiKey_Backspace;
        case VK_SPACE:
            return ImGuiKey_Space;
        case VK_RETURN:
            return ImGuiKey_Enter;
        case VK_ESCAPE:
            return ImGuiKey_Escape;
        case VK_OEM_7:
            return ImGuiKey_Apostrophe;
        case VK_OEM_COMMA:
            return ImGuiKey_Comma;
        case VK_OEM_MINUS:
            return ImGuiKey_Minus;
        case VK_OEM_PERIOD:
            return ImGuiKey_Period;
        case VK_OEM_2:
            return ImGuiKey_Slash;
        case VK_OEM_1:
            return ImGuiKey_Semicolon;
        case VK_OEM_PLUS:
            return ImGuiKey_Equal;
        case VK_OEM_4:
            return ImGuiKey_LeftBracket;
        case VK_OEM_5:
            return ImGuiKey_Backslash;
        case VK_OEM_6:
            return ImGuiKey_RightBracket;
        case VK_OEM_3:
            return ImGuiKey_GraveAccent;
        case VK_CAPITAL:
            return ImGuiKey_CapsLock;
        case VK_SCROLL:
            return ImGuiKey_ScrollLock;
        case VK_NUMLOCK:
            return ImGuiKey_NumLock;
        case VK_SNAPSHOT:
            return ImGuiKey_PrintScreen;
        case VK_PAUSE:
            return ImGuiKey_Pause;
        case VK_NUMPAD0:
            return ImGuiKey_Keypad0;
        case VK_NUMPAD1:
            return ImGuiKey_Keypad1;
        case VK_NUMPAD2:
            return ImGuiKey_Keypad2;
        case VK_NUMPAD3:
            return ImGuiKey_Keypad3;
        case VK_NUMPAD4:
            return ImGuiKey_Keypad4;
        case VK_NUMPAD5:
            return ImGuiKey_Keypad5;
        case VK_NUMPAD6:
            return ImGuiKey_Keypad6;
        case VK_NUMPAD7:
            return ImGuiKey_Keypad7;
        case VK_NUMPAD8:
            return ImGuiKey_Keypad8;
        case VK_NUMPAD9:
            return ImGuiKey_Keypad9;
        case VK_DECIMAL:
            return ImGuiKey_KeypadDecimal;
        case VK_DIVIDE:
            return ImGuiKey_KeypadDivide;
        case VK_MULTIPLY:
            return ImGuiKey_KeypadMultiply;
        case VK_SUBTRACT:
            return ImGuiKey_KeypadSubtract;
        case VK_ADD:
            return ImGuiKey_KeypadAdd;
        case IM_VK_KEYPAD_ENTER:
            return ImGuiKey_KeypadEnter;
        case VK_LSHIFT:
            return ImGuiKey_LeftShift;
        case VK_LCONTROL:
            return ImGuiKey_LeftCtrl;
        case VK_LMENU:
            return ImGuiKey_LeftAlt;
        case VK_LWIN:
            return ImGuiKey_LeftSuper;
        case VK_RSHIFT:
            return ImGuiKey_RightShift;
        case VK_RCONTROL:
            return ImGuiKey_RightCtrl;
        case VK_RMENU:
            return ImGuiKey_RightAlt;
        case VK_RWIN:
            return ImGuiKey_RightSuper;
        case VK_APPS:
            return ImGuiKey_Menu;
        case '0':
            return ImGuiKey_0;
        case '1':
            return ImGuiKey_1;
        case '2':
            return ImGuiKey_2;
        case '3':
            return ImGuiKey_3;
        case '4':
            return ImGuiKey_4;
        case '5':
            return ImGuiKey_5;
        case '6':
            return ImGuiKey_6;
        case '7':
            return ImGuiKey_7;
        case '8':
            return ImGuiKey_8;
        case '9':
            return ImGuiKey_9;
        case 'A':
            return ImGuiKey_A;
        case 'B':
            return ImGuiKey_B;
        case 'C':
            return ImGuiKey_C;
        case 'D':
            return ImGuiKey_D;
        case 'E':
            return ImGuiKey_E;
        case 'F':
            return ImGuiKey_F;
        case 'G':
            return ImGuiKey_G;
        case 'H':
            return ImGuiKey_H;
        case 'I':
            return ImGuiKey_I;
        case 'J':
            return ImGuiKey_J;
        case 'K':
            return ImGuiKey_K;
        case 'L':
            return ImGuiKey_L;
        case 'M':
            return ImGuiKey_M;
        case 'N':
            return ImGuiKey_N;
        case 'O':
            return ImGuiKey_O;
        case 'P':
            return ImGuiKey_P;
        case 'Q':
            return ImGuiKey_Q;
        case 'R':
            return ImGuiKey_R;
        case 'S':
            return ImGuiKey_S;
        case 'T':
            return ImGuiKey_T;
        case 'U':
            return ImGuiKey_U;
        case 'V':
            return ImGuiKey_V;
        case 'W':
            return ImGuiKey_W;
        case 'X':
            return ImGuiKey_X;
        case 'Y':
            return ImGuiKey_Y;
        case 'Z':
            return ImGuiKey_Z;
        case VK_F1:
            return ImGuiKey_F1;
        case VK_F2:
            return ImGuiKey_F2;
        case VK_F3:
            return ImGuiKey_F3;
        case VK_F4:
            return ImGuiKey_F4;
        case VK_F5:
            return ImGuiKey_F5;
        case VK_F6:
            return ImGuiKey_F6;
        case VK_F7:
            return ImGuiKey_F7;
        case VK_F8:
            return ImGuiKey_F8;
        case VK_F9:
            return ImGuiKey_F9;
        case VK_F10:
            return ImGuiKey_F10;
        case VK_F11:
            return ImGuiKey_F11;
        case VK_F12:
            return ImGuiKey_F12;
        default:
            return ImGuiKey_None;
    }
}

// For some reason, RE::CharEvent isn't in the header files, so define a copy
class CharEvent : public RE::InputEvent {
public:
    uint32_t keyCode;  // 18 (ascii code)
};

RE::BSEventNotifyControl InputListener::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) {
    if (!a_event) return RE::BSEventNotifyControl::kContinue;
    if (!g_showImGui) return RE::BSEventNotifyControl::kContinue;

    auto& io = ImGui::GetIO();

    for (auto event = *a_event; event; event = event->next) {
        if (event->eventType == RE::INPUT_EVENT_TYPE::kChar) {
            io.AddInputCharacter(static_cast<CharEvent*>(event)->keyCode);
        } else if (event->eventType == RE::INPUT_EVENT_TYPE::kButton) {
            const auto button = static_cast<RE::ButtonEvent*>(event);
            if (!button || (button->IsPressed() && !button->IsDown())) continue;

            auto scan_code = button->GetIDCode();
            uint32_t key = MapVirtualKeyEx(scan_code, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
            switch (scan_code) {
                case DIK_LEFTARROW:
                    key = VK_LEFT;
                    break;
                case DIK_RIGHTARROW:
                    key = VK_RIGHT;
                    break;
                case DIK_UPARROW:
                    key = VK_UP;
                    break;
                case DIK_DOWNARROW:
                    key = VK_DOWN;
                    break;
                case DIK_DELETE:
                    key = VK_DELETE;
                    break;
                case DIK_END:
                    key = VK_END;
                    break;
                case DIK_HOME:
                    key = VK_HOME;
                    break;  // pos1
                case DIK_PRIOR:
                    key = VK_PRIOR;
                    break;  // page up
                case DIK_NEXT:
                    key = VK_NEXT;
                    break;  // page down
                case DIK_INSERT:
                    key = VK_INSERT;
                    break;
                case DIK_NUMPAD0:
                    key = VK_NUMPAD0;
                    break;
                case DIK_NUMPAD1:
                    key = VK_NUMPAD1;
                    break;
                case DIK_NUMPAD2:
                    key = VK_NUMPAD2;
                    break;
                case DIK_NUMPAD3:
                    key = VK_NUMPAD3;
                    break;
                case DIK_NUMPAD4:
                    key = VK_NUMPAD4;
                    break;
                case DIK_NUMPAD5:
                    key = VK_NUMPAD5;
                    break;
                case DIK_NUMPAD6:
                    key = VK_NUMPAD6;
                    break;
                case DIK_NUMPAD7:
                    key = VK_NUMPAD7;
                    break;
                case DIK_NUMPAD8:
                    key = VK_NUMPAD8;
                    break;
                case DIK_NUMPAD9:
                    key = VK_NUMPAD9;
                    break;
                case DIK_DECIMAL:
                    key = VK_DECIMAL;
                    break;
                case DIK_NUMPADENTER:
                    key = IM_VK_KEYPAD_ENTER;
                    break;
                case DIK_RMENU:
                    key = VK_RMENU;
                    break;  // right alt
                case DIK_RCONTROL:
                    key = VK_RCONTROL;
                    break;  // right control
                case DIK_LWIN:
                    key = VK_LWIN;
                    break;  // left win
                case DIK_RWIN:
                    key = VK_RWIN;
                    break;  // right win
                case DIK_APPS:
                    key = VK_APPS;
                    break;
                default:
                    break;
            }

            switch (button->device.get()) {
                case RE::INPUT_DEVICE::kMouse:
                    if (scan_code > 7)  // middle scroll
                        io.AddMouseWheelEvent(0, button->Value() * (scan_code == 8 ? 1 : -1));
                    else {
                        if (scan_code > 5) scan_code = 5;
                        io.AddMouseButtonEvent(scan_code, button->IsPressed());
                    }
                    break;
                case RE::INPUT_DEVICE::kKeyboard:
                    io.AddKeyEvent(ImGui_ImplWin32_VirtualKeyToImGuiKey(key), button->IsPressed());
                    break;
                case RE::INPUT_DEVICE::kGamepad:
                    // not implemented yet
                    // key = GetGamepadIndex((RE::BSWin32GamepadDevice::Key)key);
                    break;
                default:
                    continue;
            }
        }
    }

    return RE::BSEventNotifyControl::kContinue;
}

// How to block input when imgui open taken from Open Animation Replacer
// https://www.nexusmods.com/skyrimspecialedition/mods/92109
// https://github.com/ersh1/OpenAnimationReplacer/tree/main

struct InputFunc {
    static constexpr auto id = REL::VariantID(67315, 68617, 0xC519E0);
    static constexpr auto offset = REL::VariantOffset(0x7B, 0x7B, 0x81);

    static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events) {
        if (a_events) {
            InputListener::GetSingleton()->ProcessEvent(a_events, a_dispatcher);
        }

        bool block = false;

        if (!g_showImGui)
            block = false;
        else {
            bool isAltDown = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);

            if (!(block = g_blockInput) && (isAltDown || g_blockClicks)) {
                if (a_events) {
                    bool hasClick = false;
                    bool hasMove = false;

                    for (auto event = *a_events; event && !hasClick; event = event->next) {
                        if (event->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                            if (const auto button = static_cast<RE::ButtonEvent*>(event)) {
                                switch (button->device.get()) {
                                    case RE::INPUT_DEVICE::kMouse:
                                        hasClick = true;
                                        break;
                                }
                            }
                        } else if (event->eventType == RE::INPUT_EVENT_TYPE::kMouseMove)
                            hasMove = true;
                    }

                    if ((g_blockClicks && hasClick) || (isAltDown && hasMove)) block = true;
                }
            }
        }

        if (block) {
            constexpr RE::InputEvent* const dummy[] = {nullptr};
            func(a_dispatcher, dummy);

            RE::PlayerCamera::GetSingleton()->idleTimer = 0;  // Force reset idle to prevent vanity camera

        } else
            func(a_dispatcher, a_events);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

///////////////////////////////////////////////////////////////
// Integration entry point

bool ImGuiIntegration::Start(void callback()) {
    logger::trace("Adding hooks for ImGui integration");

    g_RenderCallback = callback;

    SKSE::AllocTrampoline(14 * 2);

    // D3DInitHook::post_init_callbacks.push_back(postInitCallback);
    logger::trace("Adding D3DInit hook");
    write_thunk_call<D3DInitHook>();

    logger::trace("Adding DXGIPresent hook");
    write_thunk_call<DXGIPresentHook>();

    SKSE::AllocTrampoline(14);

    logger::trace("Adding input hook");
    write_thunk_call<InputFunc>();

    ImGui_ImplWin32_EnableDpiAwareness();

    return true;
}

void ImGuiIntegration::Show(bool toShow) {
    auto& io = ImGui::GetIO();

    io.MouseDrawCursor = toShow;
    io.ClearInputCharacters();
    io.ClearInputKeys();

    g_showImGui = toShow;
}

void ImGuiIntegration::BlockInput(bool toBlock, bool toBlockClicks) {
    g_blockInput = toBlock;
    g_blockClicks = toBlockClicks;
}

void ImGuiIntegration::LoadFont(void callback()) { g_LoadFontCallback = callback; }
