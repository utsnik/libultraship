/*
 * Implementation half of the SDL3-over-SDL2 shim for the Wii U.
 *
 * Only two SDL3 areas are real architecture changes rather than renames, so only
 * those need code: properties (new in SDL3) and the audio stream model (SDL3
 * replaced SDL2's callback/queue design). Everything else is handled by the
 * macros in SDL3/SDL.h.
 */
#ifdef ENABLE_GX2

#include <SDL3/SDL.h>

// This translation unit implements the shim, so it must reach SDL2's real
// SDL_AddTimer rather than the compat macro that redirects callers to us.
#ifdef SDL_AddTimer
#undef SDL_AddTimer
#endif
#ifdef SDL_PauseAudioDevice
#undef SDL_PauseAudioDevice
#endif

#include <map>
#include <mutex>
#include <string>
#include <variant>

namespace {

// ---- properties -----------------------------------------------------------
using PropValue = std::variant<Sint64, std::string, void*>;
using PropTable = std::map<std::string, PropValue>;

std::mutex gPropMutex;
std::map<SDL_PropertiesID, PropTable> gProps;
SDL_PropertiesID gNextPropId = 1;

// ---- audio ----------------------------------------------------------------
// SDL2's queued audio is a good match for what libultraship asks of
// SDL_AudioStream: push interleaved frames, ask how much is still queued.
struct AudioStreamImpl {
    SDL_AudioDeviceID device = 0;
    int bytesPerFrame = 4;
};

} // namespace

struct SDL_AudioStream : public AudioStreamImpl {};

extern "C" {

SDL_PropertiesID SDL_CreateProperties(void) {
    std::lock_guard<std::mutex> lock(gPropMutex);
    const SDL_PropertiesID id = gNextPropId++;
    gProps[id] = PropTable{};
    return id;
}

void SDL_DestroyProperties(SDL_PropertiesID props) {
    std::lock_guard<std::mutex> lock(gPropMutex);
    gProps.erase(props);
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 value) {
    std::lock_guard<std::mutex> lock(gPropMutex);
    auto it = gProps.find(props);
    if (it == gProps.end()) {
        return false;
    }
    it->second[name] = value;
    return true;
}

bool SDL_SetStringProperty(SDL_PropertiesID props, const char* name, const char* value) {
    std::lock_guard<std::mutex> lock(gPropMutex);
    auto it = gProps.find(props);
    if (it == gProps.end()) {
        return false;
    }
    it->second[name] = std::string(value != nullptr ? value : "");
    return true;
}

Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 defaultValue) {
    std::lock_guard<std::mutex> lock(gPropMutex);
    auto it = gProps.find(props);
    if (it == gProps.end()) {
        return defaultValue;
    }
    auto v = it->second.find(name);
    if (v == it->second.end() || !std::holds_alternative<Sint64>(v->second)) {
        return defaultValue;
    }
    return std::get<Sint64>(v->second);
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char* name, bool defaultValue) {
    return SDL_GetNumberProperty(props, name, defaultValue ? 1 : 0) != 0;
}

void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* defaultValue) {
    std::lock_guard<std::mutex> lock(gPropMutex);
    auto it = gProps.find(props);
    if (it == gProps.end()) {
        return defaultValue;
    }
    auto v = it->second.find(name);
    if (v == it->second.end() || !std::holds_alternative<void*>(v->second)) {
        return defaultValue;
    }
    return std::get<void*>(v->second);
}

// The Wii U never opens an SDL gamepad or an SDL window (VPAD/WPAD and GX2 are
// used instead), so these return an empty property set rather than inventing
// capability flags that would mislead callers into probing rumble/LED support.
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad*) {
    return 0;
}
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window*) {
    return 0;
}
SDL_Window* SDL_CreateWindowWithProperties(SDL_PropertiesID) {
    return nullptr;
}

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec, void*, void*) {
    if (spec == nullptr) {
        return nullptr;
    }

    SDL_AudioSpec want{};
    want.freq = spec->freq;
    want.format = spec->format;
    want.channels = spec->channels;
    want.samples = 1024;
    want.callback = nullptr; // queued mode

    SDL_AudioSpec have{};
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) {
        return nullptr;
    }

    auto* stream = new SDL_AudioStream();
    stream->device = dev;
    stream->bytesPerFrame = SDL_AUDIO_BITSIZE(have.format) / 8 * have.channels;
    (void)devid;
    return stream;
}

void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
    if (stream == nullptr) {
        return;
    }
    SDL_CloseAudioDevice(stream->device);
    delete stream;
}

bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len) {
    if (stream == nullptr) {
        return false;
    }
    return SDL_QueueAudio(stream->device, buf, (Uint32)len) == 0;
}

int SDL_GetAudioStreamQueued(SDL_AudioStream* stream) {
    return stream != nullptr ? (int)SDL_GetQueuedAudioSize(stream->device) : 0;
}

int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream) {
    return SDL_GetAudioStreamQueued(stream);
}

SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream* stream) {
    return stream != nullptr ? stream->device : 0;
}

bool SDL_ClearAudioStream(SDL_AudioStream* stream) {
    if (stream == nullptr) {
        return false;
    }
    SDL_ClearQueuedAudio(stream->device);
    return true;
}

bool SDL_ResumeAudioDevice(SDL_AudioDeviceID devid) {
    SDL_PauseAudioDevice(devid, 0);
    return true;
}

// Named target for the single-argument SDL3 form declared in the shim header.
void SDL_PauseAudioDevice_SDL2(SDL_AudioDeviceID devid, int pauseOn) {
    SDL_PauseAudioDevice(devid, pauseOn);
}

// SDL3 returns an owned array of instance ids; SDL2 enumerates by index.
SDL_JoystickID* SDL_GetJoysticks(int* count) {
    const int n = SDL_NumJoysticks();
    if (count != nullptr) {
        *count = n;
    }
    if (n <= 0) {
        return nullptr;
    }
    auto* ids = (SDL_JoystickID*)SDL_malloc(sizeof(SDL_JoystickID) * (n + 1));
    if (ids == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < n; i++) {
        ids[i] = SDL_JoystickGetDeviceInstanceID(i);
    }
    ids[n] = 0;
    return ids;
}

// Sensors/LED are absent on the Wii U input path. Report unsupported rather than
// pretending success, so callers disable the feature instead of reading zeros.
bool SDL_GamepadHasSensor(SDL_Gamepad*, int) {
    return false;
}
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad*, int, bool) {
    return false;
}
bool SDL_GetGamepadSensorData(SDL_Gamepad*, int, float*, int) {
    return false;
}
bool SDL_SetGamepadLED(SDL_Gamepad*, Uint8, Uint8, Uint8) {
    return false;
}


// SDL2 calls back as cb(interval, param); SDL3 as cb(userdata, id, interval).
// Keep the caller's function+userdata alongside the SDL2 timer so the arguments
// can be reordered, rather than casting the pointer and corrupting the stack.
} // extern "C"

namespace {
struct TimerShim {
    SDL3_TimerCallback callback;
    void* userdata;
    SDL_TimerID id;
};

std::mutex gTimerMutex;
std::map<SDL_TimerID, TimerShim*> gTimers;

Uint32 TimerTrampoline(Uint32 interval, void* param) {
    auto* shim = static_cast<TimerShim*>(param);
    if (shim == nullptr || shim->callback == nullptr) {
        return 0;
    }
    const Uint32 next = shim->callback(shim->userdata, shim->id, interval);
    if (next == 0) {
        std::lock_guard<std::mutex> lock(gTimerMutex);
        gTimers.erase(shim->id);
        delete shim;
    }
    return next;
}
} // namespace

extern "C" {

SDL_TimerID SDL3Compat_AddTimer(Uint32 interval, SDL3_TimerCallback callback, void* userdata) {
    auto* shim = new TimerShim{ callback, userdata, 0 };
    // SDL2's SDL_AddTimer is shadowed by the compat macro, so call through the
    // real symbol name to avoid recursing into this function.
    const SDL_TimerID id = SDL_AddTimer(interval, TimerTrampoline, shim);
    if (id == 0) {
        delete shim;
        return 0;
    }
    shim->id = id;
    {
        std::lock_guard<std::mutex> lock(gTimerMutex);
        gTimers[id] = shim;
    }
    return id;
}

} // extern "C"

#endif // ENABLE_GX2
