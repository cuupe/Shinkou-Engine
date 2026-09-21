#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include "shinkou/audio/AudioSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"ShinkouAudioMixerTestWindow";
constexpr wchar_t kWindowTitle[] = L"Shinkou Audio Mixer Test";
constexpr std::size_t kBusCount = static_cast<std::size_t>(shinkou::audio::AudioBus::Count);
constexpr std::size_t kVoiceCount = 4;
constexpr std::size_t kEffectCount = 6;

constexpr int kBusVolumeBase = 100;
constexpr int kBusMuteBase = 200;
constexpr int kVoicePlayBase = 300;
constexpr int kVoiceStopBase = 400;
constexpr int kVoiceLoopBase = 500;
constexpr int kVoiceVolumeBase = 600;
constexpr int kVoicePanBase = 700;
constexpr int kPlayAll = 800;
constexpr int kStopAll = 801;
constexpr int kOpenWav = 802;
constexpr int kExternalPlay = 803;
constexpr int kExternalStop = 804;
constexpr int kExternalLoop = 805;
constexpr int kExternalBus = 806;
constexpr int kEffectTarget = 900;
constexpr int kEffectLowPass = 901;
constexpr int kEffectHighPass = 902;
constexpr int kEffectCompressor = 903;
constexpr int kEffectLimiter = 904;
constexpr int kEffectDelay = 905;
constexpr int kEffectReverb = 906;
constexpr int kEffectCutoff = 907;
constexpr int kEffectThreshold = 908;
constexpr int kEffectWet = 909;
constexpr int kEffectDelayTime = 910;
constexpr int kEffectFeedback = 911;
constexpr int kEffectApply = 912;

constexpr std::array<shinkou::audio::AudioBus, kBusCount> kBuses{
    shinkou::audio::AudioBus::Master,
    shinkou::audio::AudioBus::Music,
    shinkou::audio::AudioBus::Sfx,
    shinkou::audio::AudioBus::Voice,
    shinkou::audio::AudioBus::Ambient,
    shinkou::audio::AudioBus::UI,
};

constexpr std::array<const wchar_t*, kBusCount> kBusNames{
    L"Master", L"Music", L"SFX", L"Voice", L"Ambient", L"UI"
};

struct VoiceDefinition {
    const wchar_t* name;
    const char* shortName;
    double frequency;
    shinkou::audio::AudioBus bus;
};

constexpr std::array<VoiceDefinition, kVoiceCount> kVoiceDefinitions{
    VoiceDefinition{L"Music 220 Hz", "Music", 220.0, shinkou::audio::AudioBus::Music},
    VoiceDefinition{L"SFX 330 Hz", "SFX", 330.0, shinkou::audio::AudioBus::Sfx},
    VoiceDefinition{L"Voice 440 Hz", "Voice", 440.0, shinkou::audio::AudioBus::Voice},
    VoiceDefinition{L"Ambient 550 Hz", "Ambient", 550.0, shinkou::audio::AudioBus::Ambient},
};

struct EffectRackState {
    std::array<bool, kEffectCount> enabled{};
    int cutoffHz{12000};
    int thresholdDb{-12};
    int wetPercent{25};
    int delayMilliseconds{250};
    int feedbackPercent{35};
};

constexpr std::array<const wchar_t*, kEffectCount> kEffectNames{
    L"Low-pass", L"High-pass", L"Compressor", L"Limiter", L"Delay", L"Reverb"
};

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
}

bool write_tone_wav(const std::filesystem::path& path, double frequency, std::size_t index) {
    constexpr std::uint32_t sampleRate = 48000;
    constexpr std::uint32_t durationSeconds = 4;
    constexpr std::uint32_t frameCount = sampleRate * durationSeconds;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint32_t dataBytes = frameCount * channels * bitsPerSample / 8u;
    constexpr double twoPi = 6.283185307179586476925286766559;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(44u + dataBytes);
    const auto chunk = [&bytes](const char* text) {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<std::uint8_t>(text[i]));
    };
    chunk("RIFF");
    append_u32(bytes, 36u + dataBytes);
    chunk("WAVE");
    chunk("fmt ");
    append_u32(bytes, 16u);
    append_u16(bytes, 1u);
    append_u16(bytes, channels);
    append_u32(bytes, sampleRate);
    append_u32(bytes, sampleRate * channels * bitsPerSample / 8u);
    append_u16(bytes, channels * bitsPerSample / 8u);
    append_u16(bytes, bitsPerSample);
    chunk("data");
    append_u32(bytes, dataBytes);

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        const auto time = static_cast<double>(frame) / static_cast<double>(sampleRate);
        double sample = std::sin(twoPi * frequency * time);
        if (index == 3) {
            // A slightly richer source makes the fourth row useful when all
            // voices are active at once, without needing a renderer or DSP UI.
            sample = 0.68 * sample + 0.22 * std::sin(twoPi * frequency * 1.5 * time);
        }
        const auto value = static_cast<std::int16_t>(std::clamp(sample * 0.30, -1.0, 1.0) * 32767.0);
        append_u16(bytes, static_cast<std::uint16_t>(value));
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::filesystem::path create_fixture_root() {
    wchar_t tempPath[MAX_PATH]{};
    const auto length = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (length == 0 || length >= std::size(tempPath)) return {};

    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64());
    const auto root = std::filesystem::path(tempPath) / (L"ShinkouAudioMixer-" + suffix);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    return error ? std::filesystem::path{} : root;
}

class AudioMixerTestWindow final {
    HINSTANCE instance_{};
    HWND window_{};
    HFONT font_{};
    HFONT titleFont_{};

    HWND title_{};
    HWND intro_{};
    HWND mixerGroup_{};
    HWND voicesGroup_{};
    HWND effectsGroup_{};
    HWND externalGroup_{};
    HWND diagnosticsGroup_{};
    HWND status_{};
    HWND diagnostics_{};
    HWND error_{};

    std::array<HWND, kBusCount> busLabels_{};
    std::array<HWND, kBusCount> busSliders_{};
    std::array<HWND, kBusCount> busMute_{};
    std::array<HWND, kBusCount> busValues_{};
    std::array<HWND, kVoiceCount> voicePlay_{};
    std::array<HWND, kVoiceCount> voiceStop_{};
    std::array<HWND, kVoiceCount> voiceLoop_{};
    std::array<HWND, kVoiceCount> voiceVolume_{};
    std::array<HWND, kVoiceCount> voicePan_{};
    std::array<HWND, kVoiceCount> voiceVolumeValues_{};
    std::array<HWND, kVoiceCount> voicePanValues_{};

    HWND playAll_{};
    HWND stopAll_{};
    HWND externalPathControl_{};
    HWND openWav_{};
    HWND externalPlay_{};
    HWND externalStop_{};
    HWND externalLoop_{};
    HWND externalBus_{};

    HWND effectTarget_{};
    HWND effectTargetLabel_{};
    std::array<HWND, kEffectCount> effectChecks_{};
    HWND effectCutoffLabel_{};
    HWND effectCutoff_{};
    HWND effectCutoffValue_{};
    HWND effectThresholdLabel_{};
    HWND effectThreshold_{};
    HWND effectThresholdValue_{};
    HWND effectWetLabel_{};
    HWND effectWet_{};
    HWND effectWetValue_{};
    HWND effectDelayTimeLabel_{};
    HWND effectDelayTime_{};
    HWND effectDelayTimeValue_{};
    HWND effectFeedbackLabel_{};
    HWND effectFeedback_{};
    HWND effectFeedbackValue_{};
    HWND effectApply_{};

    std::array<HWND, kVoiceCount> voiceLabels_{};
    std::unique_ptr<shinkou::audio::AudioSystem> audio_{};
    std::filesystem::path fixtureRoot_{};
    std::array<shinkou::audio::AudioAssetId, kVoiceCount> voiceAssets_{};
    std::array<shinkou::audio::AudioTrackId, kVoiceCount> voiceTracks_{};
    std::array<shinkou::audio::AudioVoiceId, kVoiceCount> voices_{};
    std::array<float, kVoiceCount> voiceVolumes_{};
    std::array<float, kVoiceCount> voicePans_{};
    std::array<bool, kVoiceCount> voiceLoops_{};
    shinkou::audio::AudioAssetId externalAsset_{0};
    shinkou::audio::AudioVoiceId externalVoice_{0};
    std::filesystem::path externalFilePath_{};
    std::array<EffectRackState, kBusCount> effectStates_{};
    std::size_t effectSelectedBus_{0};
    std::wstring statusMessage_{};
    std::wstring errorMessage_{};

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<AudioMixerTestWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<AudioMixerTestWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handle_message(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND create_control(const wchar_t* className, const wchar_t* text, DWORD style, int id) {
        auto control = CreateWindowExW(
            0, className, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return control;
    }

    HWND create_button(const wchar_t* text, int id, DWORD style = BS_PUSHBUTTON) {
        return create_control(L"BUTTON", text, style, id);
    }

    HWND create_label(const wchar_t* text, DWORD style = SS_LEFT) {
        return create_control(L"STATIC", text, style, 0);
    }

    HWND create_slider(int id, int minimum, int maximum, int value) {
        const auto slider = create_control(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | TBS_HORZ, id);
        if (!slider) return nullptr;
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(minimum, maximum));
        SendMessageW(slider, TBM_SETPOS, TRUE, value);
        return slider;
    }

    static void move_control(HWND control, int x, int y, int width, int height) {
        if (control) SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void set_text(HWND control, const std::wstring& text) {
        if (control) SetWindowTextW(control, text.c_str());
    }

    void layout() {
        RECT client{};
        GetClientRect(window_, &client);
        const int width = std::max(900L, client.right - client.left);
        const int margin = 18;

        move_control(title_, margin, 12, width - margin * 2, 28);
        move_control(intro_, margin, 43, width - margin * 2, 32);

        const int mixerTop = 80;
        const int mixerHeight = 182;
        move_control(mixerGroup_, margin, mixerTop, width - margin * 2, mixerHeight);
        const int busSliderWidth = std::max(220, width - 365);
        for (std::size_t i = 0; i < kBusCount; ++i) {
            const int y = mixerTop + 25 + static_cast<int>(i) * 25;
            move_control(busLabels_[i], 30, y + 2, 88, 20);
            move_control(busSliders_[i], 126, y, busSliderWidth, 24);
            move_control(busMute_[i], 126 + busSliderWidth + 14, y, 64, 24);
            move_control(busValues_[i], 126 + busSliderWidth + 85, y + 2, 60, 20);
        }

        const int voicesTop = mixerTop + mixerHeight + 12;
        const int voicesHeight = 250;
        move_control(voicesGroup_, margin, voicesTop, width - margin * 2, voicesHeight);
        move_control(playAll_, width - 190, voicesTop + 8, 78, 24);
        move_control(stopAll_, width - 105, voicesTop + 8, 78, 24);
        const int voiceVolumeWidth = std::max(105, (width - 760) / 2 + 130);
        const int voicePanWidth = std::max(105, (width - 760) / 2 + 130);
        for (std::size_t i = 0; i < kVoiceCount; ++i) {
            const int y = voicesTop + 43 + static_cast<int>(i) * 47;
            move_control(voiceLabels_[i], 30, y + 2, 88, 20);
            move_control(voicePlay_[i], 126, y, 48, 24);
            move_control(voiceStop_[i], 178, y, 48, 24);
            move_control(voiceVolume_[i], 280, y, voiceVolumeWidth, 24);
            move_control(voiceVolumeValues_[i], 280 + voiceVolumeWidth + 4, y + 2, 42, 20);
            const int panX = 280 + voiceVolumeWidth + 62;
            move_control(voicePan_[i], panX, y, voicePanWidth, 24);
            move_control(voicePanValues_[i], panX + voicePanWidth + 4, y + 2, 48, 20);
            move_control(voiceLoop_[i], panX + voicePanWidth + 62, y, 74, 24);
        }

        const int effectsTop = voicesTop + voicesHeight + 12;
        const int effectsHeight = 180;
        move_control(effectsGroup_, margin, effectsTop, width - margin * 2, effectsHeight);
        move_control(effectTargetLabel_, 30, effectsTop + 27, 55, 20);
        move_control(effectTarget_, 90, effectsTop + 24, 135, 23);
        move_control(effectApply_, width - 108, effectsTop + 23, 78, 25);
        for (std::size_t i = 0; i < kEffectCount; ++i) {
            const int x = 30 + static_cast<int>(i) * 108;
            move_control(effectChecks_[i], x, effectsTop + 55, 101, 23);
        }
        move_control(effectCutoffLabel_, 30, effectsTop + 94, 58, 20);
        move_control(effectCutoff_, 92, effectsTop + 91, 205, 24);
        move_control(effectCutoffValue_, 303, effectsTop + 93, 62, 20);
        move_control(effectThresholdLabel_, 390, effectsTop + 94, 65, 20);
        move_control(effectThreshold_, 460, effectsTop + 91, 150, 24);
        move_control(effectThresholdValue_, 616, effectsTop + 93, 55, 20);
        move_control(effectWetLabel_, 30, effectsTop + 133, 58, 20);
        move_control(effectWet_, 92, effectsTop + 130, 205, 24);
        move_control(effectWetValue_, 303, effectsTop + 132, 62, 20);
        move_control(effectDelayTimeLabel_, 390, effectsTop + 133, 65, 20);
        move_control(effectDelayTime_, 460, effectsTop + 130, 150, 24);
        move_control(effectDelayTimeValue_, 616, effectsTop + 132, 62, 20);
        const int feedbackLabelX = std::max(675, width - 225);
        const int feedbackSliderX = feedbackLabelX + 66;
        move_control(effectFeedbackLabel_, feedbackLabelX, effectsTop + 133, 62, 20);
        move_control(effectFeedback_, feedbackSliderX, effectsTop + 130, std::max(85, width - feedbackSliderX - 62), 24);
        move_control(effectFeedbackValue_, width - 55, effectsTop + 132, 50, 20);

        const int externalTop = effectsTop + effectsHeight + 12;
        const int externalHeight = 88;
        move_control(externalGroup_, margin, externalTop, width - margin * 2, externalHeight);
        move_control(externalPathControl_, 30, externalTop + 26, width - 275, 23);
        move_control(openWav_, width - 232, externalTop + 25, 82, 25);
        move_control(externalBus_, 30, externalTop + 56, 120, 23);
        move_control(externalLoop_, 163, externalTop + 55, 70, 24);
        move_control(externalPlay_, 248, externalTop + 55, 70, 24);
        move_control(externalStop_, 325, externalTop + 55, 70, 24);

        const int diagnosticsTop = externalTop + externalHeight + 12;
        move_control(diagnosticsGroup_, margin, diagnosticsTop, width - margin * 2, 74);
        move_control(status_, 30, diagnosticsTop + 25, width - 60, 20);
        move_control(diagnostics_, 30, diagnosticsTop + 45, width - 60, 20);
        move_control(error_, 30, diagnosticsTop + 65, width - 60, 20);
    }

    void set_error(const std::wstring& message) {
        errorMessage_ = message;
        set_text(error_, errorMessage_);
    }

    void set_status(const std::wstring& message) {
        statusMessage_ = message;
        set_text(status_, statusMessage_);
    }

    void update_bus_value(std::size_t index) {
        const auto position = static_cast<int>(SendMessageW(busSliders_[index], TBM_GETPOS, 0, 0));
        set_text(busValues_[index], std::to_wstring(position) + L"%");
    }

    void update_voice_value(std::size_t index) {
        const auto volume = static_cast<int>(SendMessageW(voiceVolume_[index], TBM_GETPOS, 0, 0));
        const auto pan = static_cast<int>(SendMessageW(voicePan_[index], TBM_GETPOS, 0, 0)) - 100;
        set_text(voiceVolumeValues_[index], std::to_wstring(volume) + L"%");
        set_text(voicePanValues_[index], pan == 0 ? L"C" : (std::to_wstring(pan) + L"%"));
    }

    std::size_t selected_effect_bus() const {
        const auto selection = static_cast<int>(SendMessageW(effectTarget_, CB_GETCURSEL, 0, 0));
        return static_cast<std::size_t>(std::clamp(selection, 0, static_cast<int>(kBusCount - 1)));
    }

    void update_effect_value_labels() {
        set_text(effectCutoffValue_, std::to_wstring(SendMessageW(effectCutoff_, TBM_GETPOS, 0, 0)) + L" Hz");
        set_text(effectThresholdValue_, std::to_wstring(SendMessageW(effectThreshold_, TBM_GETPOS, 0, 0)) + L" dB");
        set_text(effectWetValue_, std::to_wstring(SendMessageW(effectWet_, TBM_GETPOS, 0, 0)) + L"%");
        set_text(effectDelayTimeValue_, std::to_wstring(SendMessageW(effectDelayTime_, TBM_GETPOS, 0, 0)) + L" ms");
        set_text(effectFeedbackValue_, std::to_wstring(SendMessageW(effectFeedback_, TBM_GETPOS, 0, 0)) + L"%");
    }

    void store_effect_state() {
        auto& state = effectStates_[effectSelectedBus_];
        for (std::size_t index = 0; index < kEffectCount; ++index) {
            auto& check = state.enabled[index];
            check = SendMessageW(effectChecks_[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
        state.cutoffHz = static_cast<int>(SendMessageW(effectCutoff_, TBM_GETPOS, 0, 0));
        state.thresholdDb = static_cast<int>(SendMessageW(effectThreshold_, TBM_GETPOS, 0, 0));
        state.wetPercent = static_cast<int>(SendMessageW(effectWet_, TBM_GETPOS, 0, 0));
        state.delayMilliseconds = static_cast<int>(SendMessageW(effectDelayTime_, TBM_GETPOS, 0, 0));
        state.feedbackPercent = static_cast<int>(SendMessageW(effectFeedback_, TBM_GETPOS, 0, 0));
    }

    void load_effect_state(std::size_t bus) {
        const auto& state = effectStates_[bus];
        for (std::size_t i = 0; i < kEffectCount; ++i)
            SendMessageW(effectChecks_[i], BM_SETCHECK, state.enabled[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(effectCutoff_, TBM_SETPOS, TRUE, state.cutoffHz);
        SendMessageW(effectThreshold_, TBM_SETPOS, TRUE, state.thresholdDb);
        SendMessageW(effectWet_, TBM_SETPOS, TRUE, state.wetPercent);
        SendMessageW(effectDelayTime_, TBM_SETPOS, TRUE, state.delayMilliseconds);
        SendMessageW(effectFeedback_, TBM_SETPOS, TRUE, state.feedbackPercent);
        update_effect_value_labels();
    }

    void apply_effects() {
        store_effect_state();
        const auto bus = selected_effect_bus();
        const auto& state = effectStates_[bus];
        std::vector<shinkou::audio::AudioEffectDesc> effects;
        if (state.enabled[0]) {
            shinkou::audio::AudioEffectDesc effect;
            effect.type = shinkou::audio::AudioEffectType::LowPass;
            effect.cutoffHz = static_cast<float>(state.cutoffHz);
            effects.push_back(effect);
        }
        if (state.enabled[1]) {
            shinkou::audio::AudioEffectDesc effect;
            effect.type = shinkou::audio::AudioEffectType::HighPass;
            effect.cutoffHz = static_cast<float>(state.cutoffHz);
            effects.push_back(effect);
        }
        if (state.enabled[2]) {
            shinkou::audio::AudioEffectDesc effect;
            effect.type = shinkou::audio::AudioEffectType::Compressor;
            effect.thresholdDb = static_cast<float>(state.thresholdDb);
            effect.ratio = 4.0f;
            effects.push_back(effect);
        }
        if (state.enabled[3]) {
            shinkou::audio::AudioEffectDesc effect;
            effect.type = shinkou::audio::AudioEffectType::Limiter;
            effects.push_back(effect);
        }
        if (state.enabled[4]) {
            shinkou::audio::AudioEffectDesc effect;
            effect.type = shinkou::audio::AudioEffectType::Delay;
            effect.delaySeconds = state.delayMilliseconds / 1000.0f;
            effect.feedback = state.feedbackPercent / 100.0f;
            effect.wet = state.wetPercent / 100.0f;
            effects.push_back(effect);
        }
        if (state.enabled[5]) {
            shinkou::audio::AudioEffectDesc effect;
            effect.type = shinkou::audio::AudioEffectType::Reverb;
            effect.delaySeconds = std::max(0.08f, state.delayMilliseconds / 1000.0f);
            effect.feedback = state.feedbackPercent / 100.0f;
            effect.wet = state.wetPercent / 100.0f;
            effects.push_back(effect);
        }
        if (audio_) audio_->set_bus_effects(kBuses[bus], std::move(effects));
        set_status(std::wstring(L"已更新 ") + kBusNames[bus] + L" 总线效果链。\n");
        set_error(L"");
    }

    bool initialize_audio() {
        fixtureRoot_ = create_fixture_root();
        if (fixtureRoot_.empty()) {
            set_error(L"无法创建测试音频目录");
            return false;
        }

        shinkou::audio::AudioConfig config;
        config.maxVoices = 32;
        config.maxAssets = 16;
        config.maxTracks = 8;
        config.startDevice = true;
        config.outputChannels = 2;
        config.mixFormat.channels = 2;
        config.mixFormat.sampleRate = 48000;
        audio_ = std::make_unique<shinkou::audio::AudioSystem>(
            shinkou::audio::create_audio_backend(), config);
        if (!audio_->initialize()) {
            set_error(L"音频设备初始化失败: " + widen(audio_->last_error()));
            return false;
        }

        voiceVolumes_.fill(1.0f);
        voicePans_.fill(0.0f);
        voiceLoops_.fill(true);
        for (std::size_t i = 0; i < kVoiceCount; ++i) {
            const auto wavPath = fixtureRoot_ / (std::string("voice_") + kVoiceDefinitions[i].shortName + ".wav");
            if (!write_tone_wav(wavPath, kVoiceDefinitions[i].frequency, i)) {
                set_error(L"无法生成测试音频: " + widen(wavPath.string()));
                return false;
            }
            voiceAssets_[i] = audio_->load(wavPath, false);
            if (voiceAssets_[i] == 0) {
                set_error(L"测试音频加载失败: " + widen(audio_->last_error()));
                return false;
            }
            voiceTracks_[i] = audio_->create_track({
                kVoiceDefinitions[i].shortName, kVoiceDefinitions[i].bus, 1.0f, false});
            if (voiceTracks_[i] == 0) {
                set_error(L"无法创建音频测试轨道");
                return false;
            }
        }
        for (std::size_t i = 0; i < kBusCount; ++i) {
            audio_->set_bus_volume(kBuses[i], 1.0f);
            audio_->set_bus_muted(kBuses[i], false);
        }
        set_status(L"AudioSystem 已初始化；点击 Play all 开始听各总线的混音结果。\n");
        set_error(L"");
        return true;
    }

    void play_voice(std::size_t index) {
        if (!audio_ || voiceAssets_[index] == 0) return;
        if (voices_[index] != 0) audio_->stop(voices_[index]);
        shinkou::audio::AudioPlayParams params;
        params.bus = kVoiceDefinitions[index].bus;
        params.track = voiceTracks_[index];
        params.loop = voiceLoops_[index];
        params.volume = voiceVolumes_[index];
        params.pan = voicePans_[index];
        voices_[index] = audio_->play(voiceAssets_[index], params);
        if (voices_[index] == 0) {
            set_error(L"声部播放失败: " + widen(audio_->last_error()));
            return;
        }
        set_status(std::wstring(L"正在播放 ") + kVoiceDefinitions[index].name + L"；可叠加其它声部测试总线混音。\n");
        set_error(L"");
    }

    void stop_voice(std::size_t index) {
        if (audio_ && voices_[index] != 0) audio_->stop(voices_[index]);
        voices_[index] = 0;
    }

    void play_external() {
        if (!audio_ || externalAsset_ == 0) {
            set_error(L"请先选择一个 WAV 文件");
            return;
        }
        if (externalVoice_ != 0) audio_->stop(externalVoice_);
        const auto selection = static_cast<int>(SendMessageW(externalBus_, CB_GETCURSEL, 0, 0));
        shinkou::audio::AudioPlayParams params;
        params.bus = kBuses[static_cast<std::size_t>(std::clamp(selection, 0, static_cast<int>(kBusCount - 1)))];
        params.loop = SendMessageW(externalLoop_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        externalVoice_ = audio_->play(externalAsset_, params);
        if (externalVoice_ == 0) {
            set_error(L"外部 WAV 播放失败: " + widen(audio_->last_error()));
            return;
        }
        set_status(L"正在播放外部 WAV；可将它与上面的测试声部叠加。\n");
        set_error(L"");
    }

    void open_external_wav() {
        if (!audio_) {
            set_error(L"AudioSystem 未初始化");
            return;
        }
        wchar_t fileName[4096]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"WAV 音频 (*.wav)\0*.wav\0所有文件 (*.*)\0*.*\0";
        dialog.lpstrFile = fileName;
        dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) return;

        const std::filesystem::path path(fileName);
        const auto asset = audio_->load(path, false);
        if (asset == 0) {
            set_error(L"WAV 加载失败: " + widen(audio_->last_error()));
            return;
        }
        if (externalVoice_ != 0) audio_->stop(externalVoice_);
        if (externalAsset_ != 0 && externalAsset_ != asset) audio_->unload(externalAsset_);
        externalAsset_ = asset;
        externalVoice_ = 0;
        externalFilePath_ = path;
        set_text(externalPathControl_, externalFilePath_.wstring());
        set_status(L"已加载外部 WAV；选择总线后点击 Play WAV。\n");
        set_error(L"");
    }

    void update_audio() {
        if (!audio_) return;
        audio_->update(0.05f);
        for (auto& voice : voices_)
            if (voice != 0 && audio_->state(voice) == shinkou::audio::AudioVoiceState::Invalid) voice = 0;
        if (externalVoice_ != 0 && audio_->state(externalVoice_) == shinkou::audio::AudioVoiceState::Invalid)
            externalVoice_ = 0;

        const auto info = audio_->diagnostics();
        set_text(diagnostics_, L"活动声部: " + std::to_wstring(info.activeVoices) +
            L"    已加载资源: " + std::to_wstring(audio_->asset_count()) +
            L"    输出: 48 kHz / stereo    引擎音频线程缓冲: 512 frames");
        if (!audio_->last_error().empty() && errorMessage_.empty())
            set_error(widen(audio_->last_error()));
    }

    void handle_command(int id, int notification) {
        if (notification != BN_CLICKED && id != kExternalBus && id != kEffectTarget) return;
        if (id >= kBusMuteBase && id < kBusMuteBase + static_cast<int>(kBusCount)) {
            const auto index = static_cast<std::size_t>(id - kBusMuteBase);
            const bool muted = SendMessageW(busMute_[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (audio_) audio_->set_bus_muted(kBuses[index], muted);
            return;
        }
        if (id >= kVoicePlayBase && id < kVoicePlayBase + static_cast<int>(kVoiceCount)) {
            play_voice(static_cast<std::size_t>(id - kVoicePlayBase));
            return;
        }
        if (id >= kVoiceStopBase && id < kVoiceStopBase + static_cast<int>(kVoiceCount)) {
            stop_voice(static_cast<std::size_t>(id - kVoiceStopBase));
            return;
        }
        if (id >= kVoiceLoopBase && id < kVoiceLoopBase + static_cast<int>(kVoiceCount)) {
            const auto index = static_cast<std::size_t>(id - kVoiceLoopBase);
            voiceLoops_[index] = SendMessageW(voiceLoop_[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (audio_ && voices_[index] != 0) audio_->set_loop(voices_[index], voiceLoops_[index]);
            return;
        }
        if (id >= kEffectLowPass && id <= kEffectReverb) {
            apply_effects();
            return;
        }
        switch (id) {
        case kPlayAll:
            for (std::size_t i = 0; i < kVoiceCount; ++i) play_voice(i);
            break;
        case kStopAll:
            if (audio_) audio_->stop_all(shinkou::audio::AudioBus::Master);
            for (auto& voice : voices_) voice = 0;
            if (externalVoice_ != 0 && audio_) audio_->stop(externalVoice_);
            externalVoice_ = 0;
            set_status(L"已停止全部声部。\n");
            break;
        case kOpenWav:
            open_external_wav();
            break;
        case kExternalPlay:
            play_external();
            break;
        case kExternalStop:
            if (audio_ && externalVoice_ != 0) audio_->stop(externalVoice_);
            externalVoice_ = 0;
            break;
        case kEffectTarget:
            store_effect_state();
            effectSelectedBus_ = selected_effect_bus();
            load_effect_state(selected_effect_bus());
            apply_effects();
            break;
        case kEffectApply:
            apply_effects();
            break;
        default:
            break;
        }
    }

    void handle_scroll(HWND slider) {
        for (std::size_t i = 0; i < kBusCount; ++i) {
            if (slider == busSliders_[i]) {
                const auto value = static_cast<float>(SendMessageW(slider, TBM_GETPOS, 0, 0)) / 100.0f;
                if (audio_) audio_->set_bus_volume(kBuses[i], value);
                update_bus_value(i);
                return;
            }
        }
        for (std::size_t i = 0; i < kVoiceCount; ++i) {
            if (slider == voiceVolume_[i]) {
                voiceVolumes_[i] = static_cast<float>(SendMessageW(slider, TBM_GETPOS, 0, 0)) / 100.0f;
                if (audio_ && voices_[i] != 0) audio_->set_volume(voices_[i], voiceVolumes_[i]);
                update_voice_value(i);
                return;
            }
            if (slider == voicePan_[i]) {
                voicePans_[i] = static_cast<float>(SendMessageW(slider, TBM_GETPOS, 0, 0) - 100) / 100.0f;
                if (audio_ && voices_[i] != 0) audio_->set_pan(voices_[i], voicePans_[i]);
                update_voice_value(i);
                return;
            }
        }
        if (slider == effectCutoff_ || slider == effectThreshold_ || slider == effectWet_ ||
            slider == effectDelayTime_ || slider == effectFeedback_) {
            update_effect_value_labels();
            apply_effects();
        }
    }

    void create_controls() {
        title_ = create_label(L"Shinkou Audio Mixer Test", SS_LEFT);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        intro_ = create_label(L"纯 Win32 控件；测试引擎 AudioSystem 的总线、轨道、音量、声像与实时效果链。\n不使用任何渲染 API。", SS_LEFT);

        mixerGroup_ = create_control(L"BUTTON", L"Mixer buses", BS_GROUPBOX, 0);
        for (std::size_t i = 0; i < kBusCount; ++i) {
            busLabels_[i] = create_label(kBusNames[i]);
            busSliders_[i] = create_slider(kBusVolumeBase + static_cast<int>(i), 0, 100, 100);
            busMute_[i] = create_button(L"Mute", kBusMuteBase + static_cast<int>(i), BS_AUTOCHECKBOX);
            busValues_[i] = create_label(L"100%", SS_RIGHT);
        }

        voicesGroup_ = create_control(L"BUTTON", L"Test voices / tracks", BS_GROUPBOX, 0);
        playAll_ = create_button(L"Play all", kPlayAll);
        stopAll_ = create_button(L"Stop all", kStopAll);
        for (std::size_t i = 0; i < kVoiceCount; ++i) {
            voiceLabels_[i] = create_label(kVoiceDefinitions[i].name);
            voicePlay_[i] = create_button(L"Play", kVoicePlayBase + static_cast<int>(i));
            voiceStop_[i] = create_button(L"Stop", kVoiceStopBase + static_cast<int>(i));
            voiceVolume_[i] = create_slider(kVoiceVolumeBase + static_cast<int>(i), 0, 100, 100);
            voiceVolumeValues_[i] = create_label(L"100%", SS_RIGHT);
            voicePan_[i] = create_slider(kVoicePanBase + static_cast<int>(i), 0, 200, 100);
            voicePanValues_[i] = create_label(L"C", SS_RIGHT);
            voiceLoop_[i] = create_button(L"Loop", kVoiceLoopBase + static_cast<int>(i), BS_AUTOCHECKBOX);
            SendMessageW(voiceLoop_[i], BM_SETCHECK, BST_CHECKED, 0);
        }

        effectsGroup_ = create_control(L"BUTTON", L"Realtime effects rack", BS_GROUPBOX, 0);
        effectTargetLabel_ = create_label(L"Target");
        effectTarget_ = create_control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kEffectTarget);
        for (const auto name : kBusNames)
            SendMessageW(effectTarget_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(effectTarget_, CB_SETCURSEL, 0, 0);
        for (std::size_t i = 0; i < kEffectCount; ++i)
            effectChecks_[i] = create_button(kEffectNames[i], kEffectLowPass + static_cast<int>(i), BS_AUTOCHECKBOX);
        effectCutoffLabel_ = create_label(L"Cutoff");
        effectCutoff_ = create_slider(kEffectCutoff, 100, 16000, 12000);
        effectCutoffValue_ = create_label(L"12000 Hz", SS_RIGHT);
        effectThresholdLabel_ = create_label(L"Threshold");
        effectThreshold_ = create_slider(kEffectThreshold, -60, 0, -12);
        effectThresholdValue_ = create_label(L"-12 dB", SS_RIGHT);
        effectWetLabel_ = create_label(L"Wet");
        effectWet_ = create_slider(kEffectWet, 0, 100, 25);
        effectWetValue_ = create_label(L"25%", SS_RIGHT);
        effectDelayTimeLabel_ = create_label(L"Time");
        effectDelayTime_ = create_slider(kEffectDelayTime, 10, 1000, 250);
        effectDelayTimeValue_ = create_label(L"250 ms", SS_RIGHT);
        effectFeedbackLabel_ = create_label(L"Feedback");
        effectFeedback_ = create_slider(kEffectFeedback, 0, 95, 35);
        effectFeedbackValue_ = create_label(L"35%", SS_RIGHT);
        effectApply_ = create_button(L"Apply", kEffectApply);
        load_effect_state(0);

        externalGroup_ = create_control(L"BUTTON", L"External WAV", BS_GROUPBOX, 0);
        externalPathControl_ = create_control(L"EDIT", L"未选择文件", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, 0);
        openWav_ = create_button(L"Open WAV...", kOpenWav);
        externalBus_ = create_control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kExternalBus);
        for (const auto name : kBusNames) SendMessageW(externalBus_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(externalBus_, CB_SETCURSEL, 2, 0);
        externalLoop_ = create_button(L"Loop", kExternalLoop, BS_AUTOCHECKBOX);
        SendMessageW(externalLoop_, BM_SETCHECK, BST_CHECKED, 0);
        externalPlay_ = create_button(L"Play WAV", kExternalPlay);
        externalStop_ = create_button(L"Stop", kExternalStop);

        diagnosticsGroup_ = create_control(L"BUTTON", L"Diagnostics", BS_GROUPBOX, 0);
        status_ = create_label(L"");
        diagnostics_ = create_label(L"");
        error_ = create_label(L"");
        layout();
        for (std::size_t i = 0; i < kBusCount; ++i) update_bus_value(i);
        for (std::size_t i = 0; i < kVoiceCount; ++i) update_voice_value(i);
    }

    void shutdown_audio() {
        if (audio_) {
            audio_->stop_all(shinkou::audio::AudioBus::Master);
            audio_->shutdown();
            audio_.reset();
        }
        if (!fixtureRoot_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(fixtureRoot_, error);
            fixtureRoot_.clear();
        }
    }

    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            titleFont_ = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            create_controls();
            initialize_audio();
            SetTimer(window_, 1, 50, nullptr);
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_TIMER:
            update_audio();
            return 0;
        case WM_COMMAND:
            handle_command(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_HSCROLL:
            handle_scroll(reinterpret_cast<HWND>(lParam));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 900;
            info->ptMinTrackSize.y = 940;
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window_, 1);
            shutdown_audio();
            if (font_) DeleteObject(font_);
            if (titleFont_) DeleteObject(titleFont_);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

public:
    explicit AudioMixerTestWindow(HINSTANCE instance) : instance_(instance) {}

    int run(int showCommand) {
        INITCOMMONCONTROLSEX commonControls{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES};
        InitCommonControlsEx(&commonControls);

        WNDCLASSW windowClass{};
        windowClass.hInstance = instance_;
        windowClass.lpfnWndProc = &AudioMixerTestWindow::window_proc;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&windowClass);

        window_ = CreateWindowExW(
            0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 980,
            nullptr, nullptr, instance_, this);
        if (!window_) return 1;
        ShowWindow(window_, showCommand);
        UpdateWindow(window_);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }
};

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    AudioMixerTestWindow application(instance);
    return application.run(showCommand);
}
