#include "ship/controller/controldevice/controller/mapping/sdl/SDLRumbleMapping.h"

#include "ship/config/ConsoleVariable.h"
#include "ship/utils/StringHelper.h"
#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"

#ifdef __WIIU__
#include <algorithm>
#include <vpad/input.h>
#include <padscore/wpad.h>
#endif

namespace Ship {
#ifdef __WIIU__
namespace {
// SDL's Wii U joystick driver leaves rumble unimplemented, so drive the motors directly, as libultraship's removed
// WiiURumbleMapping did. The driver numbers the GamePad as player 0 and WPAD channels 0-3 as players 1-4.
void WiiUSetRumble(SDL_GameController* gamepad, uint8_t intensityPercentage, bool on) {
    const int player = SDL_GameControllerGetPlayerIndex(gamepad);
    if (player == 0) {
        if (!on) {
            VPADStopMotor(VPAD_CHAN_0);
            return;
        }
        // The GamePad motor plays an on/off bit pattern (at most 120 bits); spread the set bits for the intensity.
        uint8_t pattern[15] = {};
        const int32_t patternSize = sizeof(pattern) * 8;
        const float scale = ((intensityPercentage / 100.0f) * (1.0f - 0.3f)) + 0.3f;
        const int32_t bitcnt = patternSize * scale;
        for (int32_t i = 0; i < bitcnt; i++) {
            const int32_t bitpos = ((i * patternSize) / bitcnt) % patternSize;
            pattern[bitpos / 8] |= 1 << (bitpos % 8);
        }
        VPADControlMotor(VPAD_CHAN_0, pattern, patternSize);
    } else if (player >= 1 && player <= 4) {
        // Wii Remote / Pro Controller motors are on/off only.
        WPADControlMotor(static_cast<WPADChan>(player - 1), on ? TRUE : FALSE);
    }
}
} // namespace
#endif

SDLRumbleMapping::SDLRumbleMapping(uint8_t portIndex, uint8_t lowFrequencyIntensityPercentage,
                                   uint8_t highFrequencyIntensityPercentage)
    : ControllerRumbleMapping(PhysicalDeviceType::SDLGamepad, portIndex, lowFrequencyIntensityPercentage,
                              highFrequencyIntensityPercentage) {
    SetLowFrequencyIntensity(lowFrequencyIntensityPercentage);
    SetHighFrequencyIntensity(highFrequencyIntensityPercentage);
}

void SDLRumbleMapping::StartRumble() {
    for (const auto& [instanceId, gamepad] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(
             mPortIndex)) {
#ifdef __WIIU__
        WiiUSetRumble(gamepad, std::max(mLowFrequencyIntensityPercentage, mHighFrequencyIntensityPercentage), true);
#else
        SDL_GameControllerRumble(gamepad, mLowFrequencyIntensity, mHighFrequencyIntensity, 0);
#endif
    }
}

void SDLRumbleMapping::StopRumble() {
    for (const auto& [instanceId, gamepad] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(
             mPortIndex)) {
#ifdef __WIIU__
        WiiUSetRumble(gamepad, 0, false);
#else
        SDL_GameControllerRumble(gamepad, 0, 0, 0);
#endif
    }
}

void SDLRumbleMapping::SetLowFrequencyIntensity(uint8_t intensityPercentage) {
    mLowFrequencyIntensityPercentage = intensityPercentage;
    mLowFrequencyIntensity = UINT16_MAX * (intensityPercentage / 100.0f);
}

void SDLRumbleMapping::SetHighFrequencyIntensity(uint8_t intensityPercentage) {
    mHighFrequencyIntensityPercentage = intensityPercentage;
    mHighFrequencyIntensity = UINT16_MAX * (intensityPercentage / 100.0f);
}

std::string SDLRumbleMapping::GetRumbleMappingId() {
    return StringHelper::Sprintf("P%d", mPortIndex);
}

void SDLRumbleMapping::SaveToConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".RumbleMappings." + GetRumbleMappingId();
    Ship::Context::GetInstance()->GetConsoleVariables()->SetString(
        StringHelper::Sprintf("%s.RumbleMappingClass", mappingCvarKey.c_str()).c_str(), "SDLRumbleMapping");
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.LowFrequencyIntensity", mappingCvarKey.c_str()).c_str(),
        mLowFrequencyIntensityPercentage);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.HighFrequencyIntensity", mappingCvarKey.c_str()).c_str(),
        mHighFrequencyIntensityPercentage);
    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

void SDLRumbleMapping::EraseFromConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".RumbleMappings." + GetRumbleMappingId();

    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.RumbleMappingClass", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.LowFrequencyIntensity", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.HighFrequencyIntensity", mappingCvarKey.c_str()).c_str());

    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

std::string SDLRumbleMapping::GetPhysicalDeviceName() {
    return "SDL Gamepad";
}
} // namespace Ship
