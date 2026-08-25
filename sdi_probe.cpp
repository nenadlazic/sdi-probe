// sdi-probe - standalone Blackmagic DeckLink / SDI input validator.
//
// Answers, without needing ffmpeg or an encoder application:
//   1. Is the Desktop Video driver installed and loadable?
//   2. Which driver is running, and which SDK was this tool built against?
//   3. What devices exist, and which index does each map to?
//   4. Is a video signal actually LOCKED on an input, and in which format?
//   5. Does capture really work - dump raw frames to a file playable with ffmpeg.
//
// The DeckLink SDK headers are Blackmagic Design property and are NOT redistributed
// with this tool; point the build at your own SDK copy. See README.md.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "DeckLinkAPI.h"
#include "DeckLinkAPIVersion.h"

namespace
{

constexpr int kDefaultCaptureSeconds = 5;

// Exit codes, also documented in --help and README.
enum ExitCode {
    kOk = 0,        // locked signal found / frames captured
    kNoSignal = 1,  // devices present, but nothing arriving
    kNoDriver = 2,  // driver missing, no devices, or capture could not start
};

// The SDK hands out strings that the caller frees.
void releaseString(const char *s)
{
    if (s != nullptr) {
        free(const_cast<char *>(s));
    }
}

// Small RAII holder so error paths do not have to remember every Release().
template <typename T>
class ComPtr
{
   public:
    ComPtr() = default;
    explicit ComPtr(T *p) : ptr_(p) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    T *get() const { return ptr_; }
    T *operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    T **receive() { reset(); return &ptr_; }

    void reset()
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

   private:
    T *ptr_ = nullptr;
};

std::string fourcc(uint32_t v)
{
    char s[5] = {char((v >> 24) & 0xFF), char((v >> 16) & 0xFF), char((v >> 8) & 0xFF), char(v & 0xFF), '\0'};
    for (int i = 0; i < 4; i++) {
        if (s[i] < 32 || s[i] > 126) {
            s[i] = '?';
        }
    }
    return s;
}

// Maps a DeckLink pixel format to the matching ffmpeg -pixel_format name, so the tool
// can print a playback command for whatever it actually captured.
const char *ffmpegPixelFormat(BMDPixelFormat pf)
{
    switch (pf) {
        case bmdFormat8BitYUV:  return "uyvy422";
        case bmdFormat10BitYUV: return "v210";
        case bmdFormat8BitARGB: return "argb";
        case bmdFormat8BitBGRA: return "bgra";
        default:                return "uyvy422";
    }
}

// ---------------------------------------------------------------- capture

// Receives frames on the DeckLink callback thread. Counters are atomic because
// main() reads them while capture is running.
class CaptureCallback : public IDeckLinkInputCallback
{
   public:
    CaptureCallback(FILE *out, bool countAudio) : out_(out), countAudio_(countAudio) {}

    HRESULT QueryInterface(REFIID, void **v) override
    {
        if (v != nullptr) {
            *v = nullptr;
        }
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++refs_; }
    ULONG Release() override
    {
        const ULONG remaining = --refs_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode *mode,
                                    BMDDetectedVideoInputFormatFlags) override
    {
        if (mode == nullptr) {
            return S_OK;
        }
        const char *name = nullptr;
        mode->GetName(&name);
        printf("   [format changed] -> %s (%ldx%ld)\n", name != nullptr ? name : "?", mode->GetWidth(),
               mode->GetHeight());
        releaseString(name);

        width_ = mode->GetWidth();
        height_ = mode->GetHeight();
        BMDTimeValue duration = 0;
        BMDTimeScale scale = 0;
        if (mode->GetFrameRate(&duration, &scale) == S_OK && duration != 0) {
            fps_ = double(scale) / double(duration);
        }
        return S_OK;
    }

    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame *video, IDeckLinkAudioInputPacket *audio) override
    {
        if (video != nullptr) {
            if ((video->GetFlags() & bmdFrameHasNoInputSource) != 0) {
                framesWithoutSignal_++;
            } else {
                frames_++;
                width_ = video->GetWidth();
                height_ = video->GetHeight();
                pixelFormat_ = video->GetPixelFormat();
                writeFrame(video);
            }
        }
        if (audio != nullptr && countAudio_) {
            audioSamples_ += audio->GetSampleFrameCount();
        }
        return S_OK;
    }

    long frames() const { return frames_; }
    long framesWithoutSignal() const { return framesWithoutSignal_; }
    long audioSamples() const { return audioSamples_; }
    uint64_t bytesWritten() const { return bytesWritten_; }
    long width() const { return width_; }
    long height() const { return height_; }
    double fps() const { return fps_ > 0.0 ? fps_ : 25.0; }
    BMDPixelFormat pixelFormat() const { return pixelFormat_; }

   private:
    void writeFrame(IDeckLinkVideoInputFrame *video)
    {
        if (out_ == nullptr) {
            return;
        }
        void *buffer = nullptr;
        if (video->GetBytes(&buffer) != S_OK || buffer == nullptr) {
            return;
        }
        const size_t size = size_t(video->GetRowBytes()) * size_t(video->GetHeight());
        bytesWritten_ += fwrite(buffer, 1, size, out_);
    }

    FILE *out_ = nullptr;
    bool countAudio_ = false;

    std::atomic<ULONG> refs_{1};
    std::atomic<long> frames_{0};
    std::atomic<long> framesWithoutSignal_{0};
    std::atomic<long> audioSamples_{0};
    std::atomic<uint64_t> bytesWritten_{0};

    long width_ = 0;
    long height_ = 0;
    double fps_ = 0.0;
    BMDPixelFormat pixelFormat_ = bmdFormat8BitYUV;
};

// ---------------------------------------------------------------- device access

// Device indices span every card in the machine: a 4-channel card is 0-3, a second
// card continues at 4. This is the same ordering the SDK iterator gives any application,
// so the index printed here is the one to hand to whatever does the real capture.
IDeckLink *openDevice(int index, int *deviceCount)
{
    ComPtr<IDeckLinkIterator> iterator(CreateDeckLinkIteratorInstance());
    if (!iterator) {
        return nullptr;
    }

    IDeckLink *found = nullptr;
    IDeckLink *device = nullptr;
    int i = 0;
    while (iterator->Next(&device) == S_OK) {
        if (i == index) {
            found = device;  // ownership passes to the caller
        } else {
            device->Release();
        }
        i++;
    }
    if (deviceCount != nullptr) {
        *deviceCount = i;
    }
    return found;
}

void printDriverAndSdkVersion()
{
    ComPtr<IDeckLinkAPIInformation> api(CreateDeckLinkAPIInformationInstance());
    if (!api) {
        return;
    }
    const char *version = nullptr;
    if (api->GetString(BMDDeckLinkAPIVersion, &version) == S_OK) {
        printf("Driver (running Desktop Video API) : %s\n", version);
        releaseString(version);
    }
    printf("SDK (this tool was built against)  : %d.%d.%d\n", (BLACKMAGIC_DECKLINK_API_VERSION >> 24) & 0xFF,
           (BLACKMAGIC_DECKLINK_API_VERSION >> 16) & 0xFF, (BLACKMAGIC_DECKLINK_API_VERSION >> 8) & 0xFF);
}

void printDriverMissingHelp()
{
    printf("FAIL: cannot load the DeckLink API.\n");
    printf("      The Desktop Video driver is missing or not loaded.\n");
    printf("      Check:  dpkg -l | grep desktopvideo\n");
    printf("              lsmod | grep blackmagic\n");
    printf("              ls -l /dev/blackmagic*\n");
}

// Prints signal lock and the auto-detected mode. This is the part that says whether a
// cable is actually delivering something, as opposed to what the card merely supports.
bool printSignalStatus(IDeckLink *device)
{
    ComPtr<IDeckLinkStatus> status;
    if (device->QueryInterface(IID_IDeckLinkStatus, reinterpret_cast<void **>(status.receive())) != S_OK) {
        return false;
    }

    bool locked = false;
    if (status->GetFlag(bmdDeckLinkStatusVideoInputSignalLocked, &locked) == S_OK) {
        printf("   signal locked : %s\n", locked ? "YES" : "no");
    }

    int64_t mode = 0;
    if (status->GetInt(bmdDeckLinkStatusDetectedVideoInputMode, &mode) == S_OK && mode != 0) {
        printf("   detected mode : %s\n", fourcc(uint32_t(mode)).c_str());
    }
    return locked;
}

// Returns true if the device can capture. With -v also lists every supported mode,
// which is where you get the 4-character code for --mode.
bool printInputModes(IDeckLink *device, bool verbose)
{
    ComPtr<IDeckLinkInput> input;
    if (device->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void **>(input.receive())) != S_OK) {
        printf("   input         : device is not input-capable\n");
        return false;
    }

    ComPtr<IDeckLinkDisplayModeIterator> modes;
    if (input->GetDisplayModeIterator(modes.receive()) != S_OK) {
        return true;
    }

    int count = 0;
    IDeckLinkDisplayMode *mode = nullptr;
    while (modes->Next(&mode) == S_OK) {
        if (verbose) {
            const char *name = nullptr;
            mode->GetName(&name);
            BMDTimeValue duration = 0;
            BMDTimeScale scale = 0;
            mode->GetFrameRate(&duration, &scale);
            printf("      mode %-22s %4ldx%-4ld %6.2f fps  code=%s\n", name != nullptr ? name : "?", mode->GetWidth(),
                   mode->GetHeight(), duration != 0 ? double(scale) / double(duration) : 0.0,
                   fourcc(uint32_t(mode->GetDisplayMode())).c_str());
            releaseString(name);
        }
        count++;
        mode->Release();
    }
    if (!verbose) {
        printf("   input modes   : %d (use -v to list)\n", count);
    }
    return true;
}

// ---------------------------------------------------------------- commands

int commandInfo(bool verbose)
{
    ComPtr<IDeckLinkAPIInformation> api(CreateDeckLinkAPIInformationInstance());
    if (!api) {
        printDriverMissingHelp();
        return kNoDriver;
    }
    api.reset();

    printDriverAndSdkVersion();
    printf("\n");

    ComPtr<IDeckLinkIterator> iterator(CreateDeckLinkIteratorInstance());
    if (!iterator) {
        printf("No DeckLink iterator - driver not loaded.\n");
        return kNoDriver;
    }

    int index = 0;
    int lockedCount = 0;
    int inputCapableCount = 0;
    IDeckLink *raw = nullptr;
    while (iterator->Next(&raw) == S_OK) {
        ComPtr<IDeckLink> device(raw);

        const char *modelName = nullptr;
        const char *displayName = nullptr;
        device->GetModelName(&modelName);
        device->GetDisplayName(&displayName);

        const char *label = displayName != nullptr ? displayName : (modelName != nullptr ? modelName : "(unknown)");
        printf("== device_id=%-2d %s\n", index, label);
        if (displayName != nullptr && modelName != nullptr && strcmp(displayName, modelName) != 0) {
            printf("   model         : %s\n", modelName);
        }
        releaseString(modelName);
        releaseString(displayName);

        if (printSignalStatus(device.get())) {
            lockedCount++;
        }
        if (printInputModes(device.get(), verbose)) {
            inputCapableCount++;
        }
        printf("\n");
        index++;
    }

    if (index == 0) {
        printf("No DeckLink devices found.\n");
        return kNoDriver;
    }
    printf("Summary: %d device(s), %d input-capable, %d with a locked signal.\n", index, inputCapableCount,
           lockedCount);
    return lockedCount > 0 ? kOk : kNoSignal;
}

void printPlaybackHelp(const CaptureCallback &capture, const char *path)
{
    const char *pixelFormat = ffmpegPixelFormat(capture.pixelFormat());
    printf("wrote %.1f MB to %s\n", double(capture.bytesWritten()) / (1024.0 * 1024.0), path);
    printf("\nPlay it back with:\n");
    printf("  ffplay -f rawvideo -pixel_format %s -video_size %ldx%ld -framerate %.2f %s\n", pixelFormat,
           capture.width(), capture.height(), capture.fps(), path);
    printf("\nOr convert it to something normal:\n");
    printf("  ffmpeg -f rawvideo -pixel_format %s -video_size %ldx%ld -framerate %.2f -i %s -c:v libx264 out.mp4\n",
           pixelFormat, capture.width(), capture.height(), capture.fps(), path);
}

void printNoSignalHelp(int deviceIndex)
{
    printf("\nRESULT: NO USABLE SIGNAL on device_id=%d.\n", deviceIndex);
    printf("  - check the cable and that the source is actually transmitting\n");
    printf("  - check the format: run 'sdi-probe info -v' and pass --mode <code>\n");
    printf("  - try another device_id (indices span all cards in the machine)\n");
}

int commandCapture(int deviceIndex, uint32_t modeCode, int seconds, const char *outputPath, bool countAudio)
{
    int deviceCount = 0;
    ComPtr<IDeckLink> device(openDevice(deviceIndex, &deviceCount));
    if (!device) {
        if (deviceCount == 0) {
            printDriverMissingHelp();
        } else {
            printf("No device at index %d (found %d device(s)).\n", deviceIndex, deviceCount);
        }
        return kNoDriver;
    }

    ComPtr<IDeckLinkInput> input;
    if (device->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void **>(input.receive())) != S_OK) {
        printf("device_id=%d is not input-capable.\n", deviceIndex);
        return kNoDriver;
    }

    FILE *out = nullptr;
    if (outputPath != nullptr) {
        out = fopen(outputPath, "wb");
        if (out == nullptr) {
            printf("Cannot open output file %s\n", outputPath);
            return kNoDriver;
        }
    }

    // Without an explicit --mode we let the card auto-detect the incoming format;
    // the starting mode is then only a seed value.
    const bool autoDetect = (modeCode == 0);
    const BMDVideoInputFlags flags = autoDetect ? bmdVideoInputEnableFormatDetection : bmdVideoInputFlagDefault;
    const BMDDisplayMode mode = autoDetect ? BMDDisplayMode(bmdModeHD1080i50) : BMDDisplayMode(modeCode);

    ComPtr<CaptureCallback> capture(new CaptureCallback(out, countAudio));
    input->SetCallback(capture.get());

    int result = kOk;
    if (input->EnableVideoInput(mode, bmdFormat8BitYUV, flags) != S_OK) {
        printf("EnableVideoInput failed (mode=%s). Try --mode <code> from 'info -v'.\n", fourcc(mode).c_str());
        result = kNoDriver;
    } else {
        if (countAudio &&
            input->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2) != S_OK) {
            printf("   (audio input could not be enabled, continuing video-only)\n");
        }

        if (input->StartStreams() != S_OK) {
            printf("StartStreams failed.\n");
            result = kNoDriver;
        } else {
            printf("Capturing from device_id=%d for %d s%s...\n", deviceIndex, seconds,
                   autoDetect ? " (auto-detecting format)" : "");
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            input->StopStreams();

            printf("\nframes captured  : %ld\n", capture->frames());
            printf("frames w/o signal: %ld\n", capture->framesWithoutSignal());
            if (countAudio) {
                printf("audio samples    : %ld\n", capture->audioSamples());
            }

            if (capture->frames() == 0) {
                printNoSignalHelp(deviceIndex);
                result = kNoSignal;
            } else {
                printf("\nRESULT: OK - signal is being read.\n");
                if (out != nullptr) {
                    printPlaybackHelp(*capture.get(), outputPath);
                }
            }
        }
        input->DisableVideoInput();
        if (countAudio) {
            input->DisableAudioInput();
        }
    }

    input->SetCallback(nullptr);
    if (out != nullptr) {
        fclose(out);
    }
    return result;
}

// ---------------------------------------------------------------- cli

void usage(const char *program)
{
    printf(
        "sdi-probe - validate a Blackmagic DeckLink SDI input without ffmpeg or an encoder app\n"
        "\n"
        "USAGE\n"
        "  %s info [-v]                       driver, SDK, devices, signal lock (default)\n"
        "  %s capture [options]               actually read frames and prove it works\n"
        "  %s --help\n"
        "\n"
        "CAPTURE OPTIONS\n"
        "  --device <N>      device index as reported by 'info' (default 0)\n"
        "  --seconds <N>     how long to capture (default %d)\n"
        "  --out <file>      write raw frames to a file playable with ffmpeg (optional)\n"
        "  --mode <code>     force a display mode, e.g. Hi50; omit to auto-detect\n"
        "  --audio           also count incoming audio samples\n"
        "\n"
        "EXIT CODES\n"
        "  0  at least one input has a locked signal / frames were captured\n"
        "  1  devices exist but no signal is present\n"
        "  2  driver missing, no devices, or capture could not start\n"
        "\n"
        "TYPICAL SESSION\n"
        "  %s info                       # is the driver alive, which devices exist\n"
        "  %s info -v                    # list every supported mode with its code\n"
        "  %s capture --device 0 --seconds 5 --out /tmp/sdi.raw\n"
        "  ffplay -f rawvideo -pixel_format uyvy422 -video_size 1920x1080 -framerate 25 /tmp/sdi.raw\n"
        "\n"
        "IF SOMETHING IS WRONG\n"
        "  cannot load the DeckLink API   -> driver not installed/loaded:\n"
        "                                    dpkg -l | grep desktopvideo ; lsmod | grep blackmagic\n"
        "  devices listed, signal 'no'    -> cable unplugged or source not transmitting\n"
        "  frames captured but garbage    -> wrong --mode; take the code from 'info -v'\n"
        "  no device at index N           -> indices span ALL cards in the machine\n",
        program, program, program, kDefaultCaptureSeconds, program, program, program);
}

uint32_t parseFourcc(const char *s)
{
    if (s == nullptr || strlen(s) != 4) {
        return 0;
    }
    return (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | uint32_t(s[3]);
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string command = (argc > 1 && argv[1][0] != '-') ? argv[1] : "info";

    bool verbose = false;
    bool countAudio = false;
    int deviceIndex = 0;
    int seconds = kDefaultCaptureSeconds;
    const char *outputPath = nullptr;
    uint32_t modeCode = 0;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return kOk;
        }
        if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--audio") {
            countAudio = true;
        } else if (arg == "--device" && i + 1 < argc) {
            deviceIndex = atoi(argv[++i]);
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            modeCode = parseFourcc(argv[++i]);
        }
    }

    if (command == "capture") {
        return commandCapture(deviceIndex, modeCode, seconds, outputPath, countAudio);
    }
    if (command == "info") {
        return commandInfo(verbose);
    }
    usage(argv[0]);
    return kNoDriver;
}
