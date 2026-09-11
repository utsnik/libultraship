#include "libultraship/controller/controldeck/ControlDeck.h"

#include "ship/Context.h"
#include "libultraship/controller/controldevice/controller/Controller.h"
#include "libultraship/controller/controldevice/controller/mapping/ControllerDefaultMappings.h"
#include "ship/utils/StringHelper.h"
#include <imgui.h>
#include "ship/controller/controldevice/controller/mapping/mouse/WheelHandler.h"

#ifdef __vita__
#include <vitasdk.h>
#endif

#ifdef __WIIU__
// WUT and libultraship both define OSTime. Rename WUT's private declaration
// while importing the native input structures, as the GX2 backend does.
#define OSTime WiiUWUTTime
#include <vpad/input.h>
#include <padscore/kpad.h>
#undef OSTime
#include <algorithm>

namespace Ship {
namespace WiiU {
VPADStatus* GetVPADStatus(VPADReadError* error);
KPADStatus* GetKPADStatus(WPADChan chan, KPADError* error);
} // namespace WiiU
} // namespace Ship
#endif

#ifdef __WIIU__
namespace {
int8_t WiiUStickAxisToN64(float axis) {
    axis = std::clamp(axis, -1.0f, 1.0f);
    return static_cast<int8_t>(axis * 127.0f);
}

template <typename Stick>
void MapWiiULeftStick(OSContPad& pad, const Stick& stick) {
    pad.stick_x = WiiUStickAxisToN64(stick.x);
    pad.stick_y = WiiUStickAxisToN64(stick.y);
}

void MapWiiURightStick(OSContPad& pad, float x, float y) {
    constexpr float cButtonDeadzone = 0.25f;

    pad.right_stick_x = WiiUStickAxisToN64(x);
    pad.right_stick_y = WiiUStickAxisToN64(y);
    if (x < -cButtonDeadzone) {
        pad.button |= BTN_CLEFT;
    }
    if (x > cButtonDeadzone) {
        pad.button |= BTN_CRIGHT;
    }
    if (y > cButtonDeadzone) {
        pad.button |= BTN_CUP;
    }
    if (y < -cButtonDeadzone) {
        pad.button |= BTN_CDOWN;
    }
}

void MapWiiUDpad(OSContPad& pad, uint32_t buttons, uint32_t left, uint32_t right, uint32_t up, uint32_t down) {
    if (buttons & left) {
        pad.button |= BTN_DLEFT;
    }
    if (buttons & right) {
        pad.button |= BTN_DRIGHT;
    }
    if (buttons & up) {
        pad.button |= BTN_DUP;
    }
    if (buttons & down) {
        pad.button |= BTN_DDOWN;
    }
}

void MapWiiUCoreButtons(OSContPad& pad, uint32_t buttons, uint32_t a, uint32_t b, uint32_t plus,
                        uint32_t cUp, uint32_t cDown) {
    if (buttons & a) {
        pad.button |= BTN_A;
    }
    if (buttons & b) {
        pad.button |= BTN_B;
    }
    if (buttons & plus) {
        pad.button |= BTN_START;
    }
    if (buttons & cUp) {
        pad.button |= BTN_CUP;
    }
    if (buttons & cDown) {
        pad.button |= BTN_CDOWN;
    }
}

void MapWiiUGamePad(OSContPad& pad, const VPADStatus& status) {
    const uint32_t buttons = status.hold;
    MapWiiUCoreButtons(pad, buttons, VPAD_BUTTON_A, VPAD_BUTTON_B, VPAD_BUTTON_PLUS, VPAD_BUTTON_Y,
                       VPAD_BUTTON_X);
    if (buttons & (VPAD_BUTTON_ZL | VPAD_BUTTON_ZR)) {
        pad.button |= BTN_Z;
    }
    if (buttons & VPAD_BUTTON_L) {
        pad.button |= BTN_L;
    }
    if (buttons & VPAD_BUTTON_R) {
        pad.button |= BTN_R;
    }
    MapWiiUDpad(pad, buttons, VPAD_BUTTON_LEFT, VPAD_BUTTON_RIGHT, VPAD_BUTTON_UP, VPAD_BUTTON_DOWN);
    MapWiiULeftStick(pad, status.leftStick);
    MapWiiURightStick(pad, status.rightStick.x, status.rightStick.y);
}

void MapWiiRemote(OSContPad& pad, const KPADStatus& status) {
    const uint32_t buttons = status.hold;
    MapWiiUCoreButtons(pad, buttons, WPAD_BUTTON_A, WPAD_BUTTON_B, WPAD_BUTTON_PLUS, WPAD_BUTTON_2,
                       WPAD_BUTTON_1);
    MapWiiUDpad(pad, buttons, WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT, WPAD_BUTTON_UP, WPAD_BUTTON_DOWN);

    switch (status.extensionType) {
        case WPAD_EXT_NUNCHUK:
        case WPAD_EXT_MPLUS_NUNCHUK:
            MapWiiULeftStick(pad, status.nunchuk.stick);
            MapWiiUDpad(pad, status.nunchuk.hold, WPAD_NUNCHUK_STICK_EMULATION_LEFT,
                        WPAD_NUNCHUK_STICK_EMULATION_RIGHT, WPAD_NUNCHUK_STICK_EMULATION_UP,
                        WPAD_NUNCHUK_STICK_EMULATION_DOWN);
            if (status.nunchuk.hold & WPAD_NUNCHUK_BUTTON_Z) {
                pad.button |= BTN_Z;
            }
            if (status.nunchuk.hold & WPAD_NUNCHUK_BUTTON_C) {
                pad.button |= BTN_CRIGHT;
            }
            break;
        default:
            break;
    }
}

void MapWiiUClassicController(OSContPad& pad, const KPADStatus& status) {
    const uint32_t buttons = status.classic.hold;
    MapWiiUCoreButtons(pad, buttons, WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_B,
                       WPAD_CLASSIC_BUTTON_PLUS, WPAD_CLASSIC_BUTTON_Y, WPAD_CLASSIC_BUTTON_X);
    if (buttons & (WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_ZR)) {
        pad.button |= BTN_Z;
    }
    if (buttons & WPAD_CLASSIC_BUTTON_L) {
        pad.button |= BTN_L;
    }
    if (buttons & WPAD_CLASSIC_BUTTON_R) {
        pad.button |= BTN_R;
    }
    MapWiiUDpad(pad, buttons, WPAD_CLASSIC_BUTTON_LEFT, WPAD_CLASSIC_BUTTON_RIGHT, WPAD_CLASSIC_BUTTON_UP,
                WPAD_CLASSIC_BUTTON_DOWN);
    MapWiiULeftStick(pad, status.classic.leftStick);
    MapWiiURightStick(pad, status.classic.rightStick.x, status.classic.rightStick.y);
}

void MapWiiUProController(OSContPad& pad, const KPADStatus& status) {
    const uint32_t buttons = status.pro.hold;
    MapWiiUCoreButtons(pad, buttons, WPAD_PRO_BUTTON_A, WPAD_PRO_BUTTON_B, WPAD_PRO_BUTTON_PLUS,
                       WPAD_PRO_BUTTON_Y, WPAD_PRO_BUTTON_X);
    if (buttons & (WPAD_PRO_TRIGGER_ZL | WPAD_PRO_TRIGGER_ZR)) {
        pad.button |= BTN_Z;
    }
    if (buttons & WPAD_PRO_TRIGGER_L) {
        pad.button |= BTN_L;
    }
    if (buttons & WPAD_PRO_TRIGGER_R) {
        pad.button |= BTN_R;
    }
    MapWiiUDpad(pad, buttons, WPAD_PRO_BUTTON_LEFT, WPAD_PRO_BUTTON_RIGHT, WPAD_PRO_BUTTON_UP,
                WPAD_PRO_BUTTON_DOWN);
    MapWiiULeftStick(pad, status.pro.leftStick);
    MapWiiURightStick(pad, status.pro.rightStick.x, status.pro.rightStick.y);
}

void MapWiiUKpad(OSContPad& pad, const KPADStatus& status) {
    switch (status.extensionType) {
        case WPAD_EXT_PRO_CONTROLLER:
            MapWiiUProController(pad, status);
            break;
        case WPAD_EXT_CLASSIC:
        case WPAD_EXT_MPLUS_CLASSIC:
            MapWiiUClassicController(pad, status);
            break;
        case WPAD_EXT_CORE:
        case WPAD_EXT_NUNCHUK:
        case WPAD_EXT_MPLUS:
        case WPAD_EXT_MPLUS_NUNCHUK:
            MapWiiRemote(pad, status);
            break;
        default:
            break;
    }
}
} // namespace
#endif

namespace LUS {
ControlDeck::ControlDeck(std::vector<CONTROLLERBUTTONS_T> additionalBitmasks,
                         std::shared_ptr<Ship::ControllerDefaultMappings> controllerDefaultMappings,
                         std::unordered_map<CONTROLLERBUTTONS_T, std::string> buttonNames)
    : Ship::ControlDeck(additionalBitmasks, controllerDefaultMappings, buttonNames), mPads(nullptr) {
    std::vector<CONTROLLERBUTTONS_T> bitmasks;
    for (auto [bitmask, name] : buttonNames) {
        bitmasks.push_back(bitmask);
    }
    bitmasks.insert(bitmasks.end(), additionalBitmasks.begin(), additionalBitmasks.end());
    for (int32_t i = 0; i < MAXCONTROLLERS; i++) {
        mPorts.push_back(std::make_shared<Ship::ControlPort>(i, std::make_shared<Controller>(i, bitmasks)));
    }
}

ControlDeck::ControlDeck(std::vector<CONTROLLERBUTTONS_T> additionalBitmasks)
    : ControlDeck(additionalBitmasks, std::make_shared<LUS::ControllerDefaultMappings>(),
                  std::unordered_map<CONTROLLERBUTTONS_T, std::string>({
                      { BTN_A, "A" },
                      { BTN_B, "B" },
                      { BTN_L, "L" },
                      { BTN_R, "R" },
                      { BTN_Z, "Z" },
                      { BTN_START, "Start" },
                      { BTN_CLEFT, "CLeft" },
                      { BTN_CRIGHT, "CRight" },
                      { BTN_CUP, "CUp" },
                      { BTN_CDOWN, "CDown" },
                      { BTN_DLEFT, "DLeft" },
                      { BTN_DRIGHT, "DRight" },
                      { BTN_DUP, "DUp" },
                      { BTN_DDOWN, "DDown" },
                  })) {
}

ControlDeck::ControlDeck() : ControlDeck(std::vector<CONTROLLERBUTTONS_T>()) {
}

OSContPad* ControlDeck::GetPads() {
    return mPads;
}

void ControlDeck::WriteToPad(void* pad) {
    WriteToOSContPad((OSContPad*)pad);
}

void ControlDeck::WriteToOSContPad(OSContPad* pad) {
#ifdef __vita__
    // Vita has no SDL game-controller backend. Poll its single native pad and
    // expose a usable N64 layout directly to the game.
    SDL_PumpEvents();
    Ship::WheelHandler::GetInstance()->Update();

    if (AllGameInputBlocked() || pad == nullptr) {
        return;
    }

    mPads = pad;
    SceCtrlData vitaPad{};
    sceCtrlPeekBufferPositive(0, &vitaPad, 1);

    OSContPad& n64Pad = pad[0];
    const uint32_t buttons = vitaPad.buttons;
    if (buttons & SCE_CTRL_CROSS) {
        n64Pad.button |= BTN_A;
    }
    if (buttons & SCE_CTRL_CIRCLE) {
        n64Pad.button |= BTN_B;
    }
    if (buttons & SCE_CTRL_LTRIGGER) {
        n64Pad.button |= BTN_Z;
    }
    if (buttons & SCE_CTRL_RTRIGGER) {
        n64Pad.button |= BTN_R;
    }
    if (buttons & SCE_CTRL_SELECT) {
        n64Pad.button |= BTN_L;
    }
    if (buttons & SCE_CTRL_START) {
        n64Pad.button |= BTN_START;
    }

    // The Vita's face buttons and d-pad cover the N64's C and d-pad inputs;
    // overlapping the d-pad keeps both movement and camera navigation usable.
    if (buttons & SCE_CTRL_TRIANGLE) {
        n64Pad.button |= BTN_CUP;
    }
    if (buttons & SCE_CTRL_SQUARE) {
        n64Pad.button |= BTN_CDOWN;
    }
    if (buttons & SCE_CTRL_UP) {
        n64Pad.button |= BTN_DUP | BTN_CUP;
    }
    if (buttons & SCE_CTRL_DOWN) {
        n64Pad.button |= BTN_DDOWN | BTN_CDOWN;
    }
    if (buttons & SCE_CTRL_LEFT) {
        n64Pad.button |= BTN_DLEFT | BTN_CLEFT;
    }
    if (buttons & SCE_CTRL_RIGHT) {
        n64Pad.button |= BTN_DRIGHT | BTN_CRIGHT;
    }

    n64Pad.stick_x = static_cast<int8_t>(static_cast<int>(vitaPad.lx) - 128);
    n64Pad.stick_y = static_cast<int8_t>(128 - static_cast<int>(vitaPad.ly));
    return;
#elif defined(__WIIU__)
    // CafeOS has no SDL game-controller backend. The platform layer polls the
    // native devices once per frame; consume only successful samples here.
    Ship::WheelHandler::GetInstance()->Update();

    if (AllGameInputBlocked() || pad == nullptr) {
        return;
    }

    mPads = pad;
    size_t nextPad = 0;

    VPADReadError vpadError = VPAD_READ_NO_SAMPLES;
    VPADStatus* vpad = Ship::WiiU::GetVPADStatus(&vpadError);
    if (vpad != nullptr && vpadError == VPAD_READ_SUCCESS) {
        MapWiiUGamePad(pad[nextPad++], *vpad);
    }

    for (int i = 0; i < 4 && nextPad < MAXCONTROLLERS; i++) {
        KPADError kpadError = KPAD_ERROR_NO_SAMPLES;
        KPADStatus* kpad = Ship::WiiU::GetKPADStatus((WPADChan)i, &kpadError);
        if (kpad != nullptr && kpadError == KPAD_ERROR_OK) {
            MapWiiUKpad(pad[nextPad++], *kpad);
        }
    }
    return;
#else
#ifndef __WIIU__
    SDL_PumpEvents();
#endif
    Ship::WheelHandler::GetInstance()->Update();

    if (AllGameInputBlocked()) {
        return;
    }

    mPads = pad;

    for (size_t i = 0; i < mPorts.size(); i++) {
        const std::shared_ptr<Ship::Controller> controller = mPorts[i]->GetConnectedController();

        if (controller != nullptr) {
            controller->ReadToPad(&pad[i]);
        }
    }
#endif
}
} // namespace LUS
