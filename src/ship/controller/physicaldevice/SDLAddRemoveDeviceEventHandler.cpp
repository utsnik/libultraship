#include "ship/controller/physicaldevice/SDLAddRemoveDeviceEventHandler.h"
#include <SDL2/SDL.h>
#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"

namespace Ship {

SDLAddRemoveDeviceEventHandler::~SDLAddRemoveDeviceEventHandler() {
}

void SDLAddRemoveDeviceEventHandler::InitElement() {
}

void SDLAddRemoveDeviceEventHandler::DrawElement() {
}

void SDLAddRemoveDeviceEventHandler::UpdateElement() {
#ifdef __WIIU__
    // Wii U input is VPAD/KPAD, not SDL game controllers. In particular, do not
    // pump or inspect SDL controller events on the GX2 path.
    return;
#else
    SDL_PumpEvents();
    SDL_Event event;
    bool changed = false;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEADDED) > 0) {
        // from https://wiki.libsdl.org/SDL2/SDL_ControllerDeviceEvent: which - the joystick device index for
        // the SDL_CONTROLLERDEVICEADDED event
        Context::GetRawInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->HandlePhysicalDeviceConnect(
            event.cdevice.which);
        changed = true;
    }

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED, SDL_CONTROLLERDEVICEREMOVED) > 0) {
        // from https://wiki.libsdl.org/SDL2/SDL_ControllerDeviceEvent: which - the [...] instance id for the
        // SDL_CONTROLLERDEVICEREMOVED [...] event
        Context::GetRawInstance()
            ->GetControlDeck()
            ->GetConnectedPhysicalDeviceManager()
            ->HandlePhysicalDeviceDisconnect(event.cdevice.which);
        changed = true;
    }

    // The connected controller set changed, so re-point the ImGui gamepad
    // backend at it (keeps menu navigation working across hotplug).
    if (changed) {
        auto window = Context::GetRawInstance()->GetWindow();
        if (window != nullptr && window->GetGui() != nullptr) {
            window->GetGui()->RefreshImGuiGamepads();
        }
    }
#endif
}
} // namespace Ship
