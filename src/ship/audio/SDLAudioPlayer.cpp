#include "ship/audio/SDLAudioPlayer.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <spdlog/spdlog.h>

namespace {

constexpr const char* kPcmDumpPath1 = "audiodump.raw";
constexpr const char* kPcmDumpPath2 = "fs:/vol/external01/wiiu/apps/spaghettify/audiodump.raw";
constexpr size_t kPcmDumpStagingSize = 512 * 1024;
constexpr size_t kPcmDumpCap = 4 * 1024 * 1024;
static uint8_t gPcmDumpBuffer[kPcmDumpStagingSize];
static size_t gPcmDumpUsed = 0;
static size_t gPcmDumpTotal = 0;
static bool gPcmDumpDone = false;
static bool gPcmDumpTapLogged = false;
static bool gPcmDumpOpenedLogged = false;

FILE* OpenPcmDump(const char* mode, const char*& chosenPath, int& e1, int& e2) {
    FILE* fp = fopen(kPcmDumpPath1, mode);
    if (fp) {
        chosenPath = kPcmDumpPath1;
        return fp;
    }
    e1 = errno;

    fp = fopen(kPcmDumpPath2, mode);
    if (fp) {
        chosenPath = kPcmDumpPath2;
        return fp;
    }
    e2 = errno;
    return nullptr;
}

void FlushPcm() {
    const char* chosenPath = nullptr;
    int e1 = 0;
    int e2 = 0;
    const char* mode = gPcmDumpTotal == 0 ? "wb" : "ab";
    FILE* fp = OpenPcmDump(mode, chosenPath, e1, e2);
    if (!fp) {
        SPDLOG_ERROR("PROBE/PCM: fopen failed p1={} e1={} p2={} e2={}", kPcmDumpPath1, e1, kPcmDumpPath2, e2);
        gPcmDumpDone = true;
        return;
    }

    if (!gPcmDumpOpenedLogged) {
        SPDLOG_INFO("PROBE/PCM: opened path={}", chosenPath);
        gPcmDumpOpenedLogged = true;
    }
    fwrite(gPcmDumpBuffer, 1, gPcmDumpUsed, fp);
    fclose(fp);
    gPcmDumpTotal += gPcmDumpUsed;
    SPDLOG_INFO("PROBE/PCM: flushed {} of {} bytes", gPcmDumpTotal, kPcmDumpCap);
    gPcmDumpUsed = 0;
    if (gPcmDumpTotal >= kPcmDumpCap) {
        gPcmDumpDone = true;
    }
}

void CapturePcm(const uint8_t* buf, size_t len) {
    if (!gPcmDumpTapLogged) {
        SPDLOG_INFO("PROBE/PCM: tap reached, len={}", len);
        gPcmDumpTapLogged = true;
    }

    if (gPcmDumpDone) {
        return;
    }

    while (len > 0 && !gPcmDumpDone) {
        const size_t remaining = kPcmDumpStagingSize - gPcmDumpUsed;
        const size_t copySize = len < remaining ? len : remaining;
        std::memcpy(gPcmDumpBuffer + gPcmDumpUsed, buf, copySize);
        gPcmDumpUsed += copySize;
        buf += copySize;
        len -= copySize;

        if (gPcmDumpUsed == kPcmDumpStagingSize) {
            FlushPcm();
        }
    }
}

} // namespace

namespace Ship {

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: requested {} channels at {} Hz; negotiated {} channels at {} Hz; negotiated format {}; requested format {}",
                mNumChannels, this->GetSampleRate(), have.channels, have.freq, have.format, want.format);
    if (have.channels != want.channels || have.freq != want.freq || have.format != want.format) {
        SPDLOG_WARN("SDL Audio device format differs from the requested format");
    }

    SDL_PauseAudioDevice(mDevice, 0);
    return true;
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
#ifdef LUS_PCM_DUMP
    // Debug probe ONLY - blocking 512 KiB SD write from the audio path. Shipped
    // unconditionally in 161289b4 and broke Mario Kart's music. Keep it off.
    CapturePcm(buf, len);
#else
    (void)&CapturePcm;
#endif

    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        if (SDL_QueueAudio(mDevice, buf, len) != 0) {
            SPDLOG_ERROR("SDL_QueueAudio failed: {}", SDL_GetError());
        }
    }
}
} // namespace Ship
