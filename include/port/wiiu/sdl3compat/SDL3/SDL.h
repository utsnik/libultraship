/*
 * SDL3-over-SDL2 compatibility shim for the Wii U (CafeOS).
 *
 * WHY THIS EXISTS
 * ---------------
 * libultraship targets SDL3. SDL3 has no Wii U video driver upstream (there are
 * drivers for n3ds, ps2, psp and vita, but not Wii U/CafeOS) and devkitPro ships
 * only `wiiu-sdl2`. There is therefore no SDL3 to link against on this console
 * and no prospect of one appearing from a package.
 *
 * Of the 311 distinct SDL_* symbols libultraship references, 204 already exist in
 * devkitPro's SDL2. This header supplies the remaining 107 by mapping them onto
 * their SDL2 equivalents, so the shared libultraship sources compile unmodified.
 *
 * SCOPE AND HONESTY
 * -----------------
 * This is a COMPILE-COMPATIBILITY layer, not an SDL3 implementation. The Wii U
 * takes input from VPAD/WPAD through the native WiiU controller path and renders
 * through GX2, so the SDL gamepad/video surfaces here mainly need to typecheck.
 * Where SDL3 genuinely changed architecture rather than names -- the audio stream
 * API and the properties API -- small real implementations are provided below and
 * are marked as such. Nothing here silently fabricates data: unsupported calls
 * return failure or zero rather than plausible-looking values.
 */
#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_gamecontroller.h>
#include <SDL2/SDL_audio.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Events: SDL3 flattened and renamed the event enum.
 * ------------------------------------------------------------------ */
#define SDL_EVENT_FIRST SDL_FIRSTEVENT
#define SDL_EVENT_LAST SDL_LASTEVENT
#define SDL_EVENT_QUIT SDL_QUIT
#define SDL_EVENT_KEY_DOWN SDL_KEYDOWN
#define SDL_EVENT_KEY_UP SDL_KEYUP
#define SDL_EVENT_MOUSE_BUTTON_DOWN SDL_MOUSEBUTTONDOWN
#define SDL_EVENT_MOUSE_BUTTON_UP SDL_MOUSEBUTTONUP
#define SDL_EVENT_MOUSE_WHEEL SDL_MOUSEWHEEL
#define SDL_EVENT_DROP_FILE SDL_DROPFILE
#define SDL_EVENT_GAMEPAD_ADDED SDL_CONTROLLERDEVICEADDED
#define SDL_EVENT_GAMEPAD_REMOVED SDL_CONTROLLERDEVICEREMOVED
#define SDL_EVENT_WINDOW_CLOSE_REQUESTED SDL_WINDOWEVENT_CLOSE
#define SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED SDL_WINDOWEVENT_SIZE_CHANGED

/* SDL3 renamed the event union's controller-device member cdevice -> gdevice.
 * Mapping a bare identifier with the preprocessor is normally unwise, but
 * `gdevice` appears exactly twice in the tree and only ever as this member, and
 * confining the rename to the Wii U build avoids patching upstream source. */
#define gdevice cdevice

/* ------------------------------------------------------------------ *
 * Gamepad: SDL3 renamed GameController -> Gamepad wholesale.
 * ------------------------------------------------------------------ */
typedef SDL_GameController SDL_Gamepad;
typedef SDL_GameControllerAxis SDL_GamepadAxis;
typedef SDL_GameControllerButton SDL_GamepadButton;

#define SDL_INIT_GAMEPAD SDL_INIT_GAMECONTROLLER
#define SDL_OpenGamepad SDL_GameControllerOpen
#define SDL_CloseGamepad SDL_GameControllerClose
#define SDL_IsGamepad SDL_IsGameController
#define SDL_GetGamepadAxis SDL_GameControllerGetAxis
#define SDL_GetGamepadButton SDL_GameControllerGetButton
#define SDL_GetGamepadName SDL_GameControllerName
#define SDL_RumbleGamepad SDL_GameControllerRumble
#define SDL_AddGamepadMappingsFromFile SDL_GameControllerAddMappingsFromFile

#define SDL_GAMEPAD_AXIS_LEFTX SDL_CONTROLLER_AXIS_LEFTX
#define SDL_GAMEPAD_AXIS_LEFTY SDL_CONTROLLER_AXIS_LEFTY
#define SDL_GAMEPAD_AXIS_RIGHTX SDL_CONTROLLER_AXIS_RIGHTX
#define SDL_GAMEPAD_AXIS_RIGHTY SDL_CONTROLLER_AXIS_RIGHTY
#define SDL_GAMEPAD_AXIS_LEFT_TRIGGER SDL_CONTROLLER_AXIS_TRIGGERLEFT
#define SDL_GAMEPAD_AXIS_RIGHT_TRIGGER SDL_CONTROLLER_AXIS_TRIGGERRIGHT
#define SDL_GAMEPAD_AXIS_COUNT SDL_CONTROLLER_AXIS_MAX

/* SDL3 renamed the face buttons to compass directions. A/B/X/Y in SDL2 are laid
 * out Nintendo-style in the enum, so South/East/West/North map as below. */
#define SDL_GAMEPAD_BUTTON_SOUTH SDL_CONTROLLER_BUTTON_A
#define SDL_GAMEPAD_BUTTON_EAST SDL_CONTROLLER_BUTTON_B
#define SDL_GAMEPAD_BUTTON_WEST SDL_CONTROLLER_BUTTON_X
#define SDL_GAMEPAD_BUTTON_NORTH SDL_CONTROLLER_BUTTON_Y
#define SDL_GAMEPAD_BUTTON_BACK SDL_CONTROLLER_BUTTON_BACK
#define SDL_GAMEPAD_BUTTON_GUIDE SDL_CONTROLLER_BUTTON_GUIDE
#define SDL_GAMEPAD_BUTTON_START SDL_CONTROLLER_BUTTON_START
#define SDL_GAMEPAD_BUTTON_LEFT_STICK SDL_CONTROLLER_BUTTON_LEFTSTICK
#define SDL_GAMEPAD_BUTTON_RIGHT_STICK SDL_CONTROLLER_BUTTON_RIGHTSTICK
#define SDL_GAMEPAD_BUTTON_LEFT_SHOULDER SDL_CONTROLLER_BUTTON_LEFTSHOULDER
#define SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
#define SDL_GAMEPAD_BUTTON_DPAD_UP SDL_CONTROLLER_BUTTON_DPAD_UP
#define SDL_GAMEPAD_BUTTON_DPAD_DOWN SDL_CONTROLLER_BUTTON_DPAD_DOWN
#define SDL_GAMEPAD_BUTTON_DPAD_LEFT SDL_CONTROLLER_BUTTON_DPAD_LEFT
#define SDL_GAMEPAD_BUTTON_DPAD_RIGHT SDL_CONTROLLER_BUTTON_DPAD_RIGHT
#define SDL_GAMEPAD_BUTTON_MISC1 SDL_CONTROLLER_BUTTON_MISC1
#define SDL_GAMEPAD_BUTTON_LEFT_PADDLE1 SDL_CONTROLLER_BUTTON_PADDLE2
#define SDL_GAMEPAD_BUTTON_LEFT_PADDLE2 SDL_CONTROLLER_BUTTON_PADDLE4
#define SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1 SDL_CONTROLLER_BUTTON_PADDLE1
#define SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2 SDL_CONTROLLER_BUTTON_PADDLE3
#define SDL_GAMEPAD_BUTTON_COUNT SDL_CONTROLLER_BUTTON_MAX

/* ------------------------------------------------------------------ *
 * Properties: entirely new in SDL3. Implemented for real (small opaque
 * key/value store) rather than aliased, because there is no SDL2 analogue.
 * Only the getters/setters libultraship actually calls are provided.
 * ------------------------------------------------------------------ */
typedef uint32_t SDL_PropertiesID;

#ifdef __cplusplus
extern "C" {
#endif
SDL_PropertiesID SDL_CreateProperties(void);
void SDL_DestroyProperties(SDL_PropertiesID props);
bool SDL_SetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 value);
bool SDL_SetStringProperty(SDL_PropertiesID props, const char* name, const char* value);
Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 defaultValue);
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char* name, bool defaultValue);
void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* defaultValue);
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad* gamepad);
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* window);
SDL_Window* SDL_CreateWindowWithProperties(SDL_PropertiesID props);
#ifdef __cplusplus
}
#endif

#define SDL_PROP_WINDOW_CREATE_TITLE_STRING "SDL.window.create.title"
#define SDL_PROP_WINDOW_CREATE_X_NUMBER "SDL.window.create.x"
#define SDL_PROP_WINDOW_CREATE_Y_NUMBER "SDL.window.create.y"
#define SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER "SDL.window.create.width"
#define SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER "SDL.window.create.height"
#define SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER "SDL.window.create.flags"
#define SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN "SDL.gamepad.cap.rumble"
#define SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN "SDL.gamepad.cap.rgb_led"
/* Desktop-only handles; defined so shared code typechecks. Never resolved here. */
#define SDL_PROP_WINDOW_WIN32_HWND_POINTER "SDL.window.win32.hwnd"
#define SDL_PROP_WINDOW_COCOA_WINDOW_POINTER "SDL.window.cocoa.window"

/* ------------------------------------------------------------------ *
 * Audio: SDL3 replaced the callback/queue model with SDL_AudioStream.
 * This is a genuine architecture change, so the stream is implemented on
 * top of SDL2's queued-audio API rather than aliased.
 * ------------------------------------------------------------------ */
#define SDL_AUDIO_S16 AUDIO_S16SYS

/* SDL3 split pause/resume into single-argument calls; SDL2 has one function with
 * a pause_on flag. */
#ifdef SDL_PauseAudioDevice
#undef SDL_PauseAudioDevice
#endif
#define SDL_PauseAudioDevice(dev) SDL_PauseAudioDevice_SDL2((dev), 1)
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0

typedef struct SDL_AudioStream SDL_AudioStream;

#ifdef __cplusplus
extern "C" {
#endif
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec, void* callback,
                                           void* userdata);
void SDL_DestroyAudioStream(SDL_AudioStream* stream);
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len);
int SDL_GetAudioStreamQueued(SDL_AudioStream* stream);
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream* stream);
bool SDL_ClearAudioStream(SDL_AudioStream* stream);
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID devid);
void SDL_PauseAudioDevice_SDL2(SDL_AudioDeviceID devid, int pauseOn);
#ifdef __cplusplus
}
#endif

/* ------------------------------------------------------------------ *
 * Display / window / misc renames.
 * ------------------------------------------------------------------ */
typedef uint32_t SDL_DisplayID;

#define SDL_GL_DestroyContext SDL_GL_DeleteContext
#define SDL_HideCursor() SDL_ShowCursor(SDL_DISABLE)
#define SDL_GetPrimaryDisplay() ((SDL_DisplayID)0)
#define SDL_GetDisplayForWindow(w) ((SDL_DisplayID)SDL_GetWindowDisplayIndex(w))
#define SDL_GetClosestFullscreenDisplayMode SDL_GetClosestDisplayMode
#define SDL_SetWindowFullscreenMode(w, m) SDL_SetWindowDisplayMode((w), (m))
#define SDL_SetEventEnabled(type, enabled) SDL_EventState((type), (enabled) ? SDL_ENABLE : SDL_IGNORE)
#define SDL_GetWindowRelativeMouseMode(w) (SDL_GetRelativeMouseMode() == SDL_TRUE)
#define SDL_SetWindowRelativeMouseMode(w, on) SDL_SetRelativeMouseMode((on) ? SDL_TRUE : SDL_FALSE)
#define SDL_WINDOW_HIGH_PIXEL_DENSITY SDL_WINDOW_ALLOW_HIGHDPI
#define SDL_GetCurrentRenderOutputSize SDL_GetRendererOutputSize
#define SDL_SetRenderVSync(r, v) (0)

/* Joystick id enumeration was reshaped in SDL3. */
#define SDL_GetJoystickGUIDForID SDL_JoystickGetDeviceGUID
#ifdef __cplusplus
extern "C" {
#endif
SDL_JoystickID* SDL_GetJoysticks(int* count);
bool SDL_GetGamepadSensorData(SDL_Gamepad* gamepad, int type, float* data, int numValues);
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad* gamepad, int type, bool enabled);
bool SDL_GamepadHasSensor(SDL_Gamepad* gamepad, int type);
bool SDL_SetGamepadLED(SDL_Gamepad* gamepad, Uint8 red, Uint8 green, Uint8 blue);
#ifdef __cplusplus
}
#endif

/* ------------------------------------------------------------------ *
 * Timers: SDL3 reversed the callback argument order and added the timer
 * id, so this needs a real trampoline rather than an alias.
 *   SDL3: Uint32 cb(void *userdata, SDL_TimerID id, Uint32 interval)
 *   SDL2: Uint32 cb(Uint32 interval, void *param)
 * ------------------------------------------------------------------ */
typedef Uint32 (*SDL3_TimerCallback)(void* userdata, SDL_TimerID timerID, Uint32 interval);

#ifdef __cplusplus
extern "C" {
#endif
SDL_TimerID SDL3Compat_AddTimer(Uint32 interval, SDL3_TimerCallback callback, void* userdata);
#ifdef __cplusplus
}
#endif

/* Route SDL_AddTimer to the trampoline. Undef first: SDL2 declares it already. */
#ifdef SDL_AddTimer
#undef SDL_AddTimer
#endif
#define SDL_AddTimer(interval, callback, userdata) SDL3Compat_AddTimer((interval), (callback), (userdata))
