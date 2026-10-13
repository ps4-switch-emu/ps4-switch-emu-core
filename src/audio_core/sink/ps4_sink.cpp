// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

#include <orbis/AudioOut.h>

#include "audio_core/common/common.h"
#include "audio_core/sink/ps4_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/alignment.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"

extern "C" __attribute__((weak)) void ps4_sound_close();                // externals/ps4sup/ps4_sounds.cpp
extern "C" __attribute__((weak)) void ps4_boot_trace(const char* stage); // externals/ps4sup

namespace AudioCore::Sink {

/// @brief PS4 sink stream, responsible for sinking samples to hardware.
struct PS4SinkStream final : public SinkStream {
    /// @brief Create a new sink stream.
    /// @param device_channels_ - Number of channels supported by the hardware.
    /// @param system_channels_ - Number of channels the audio systems expect.
    /// @param output_device - Name of the output device to use for this stream.
    /// @param input_device - Name of the input device to use for this stream.
    /// @param type_ - Type of this stream.
    /// @param system_ - Core system.
    /// @param event - Event used only for audio renderer, signalled on buffer consume.
    PS4SinkStream(u32 device_channels_, u32 system_channels_, const std::string& output_device, const std::string& input_device, StreamType type_, Core::System& system_)
        : SinkStream{system_, type_}
    {
        system_channels = system_channels_;
        device_channels = device_channels_;

        constexpr size_t PREFERRED_SAMPLE_COUNT = 1024;
        if (type == StreamType::In) {
            // No microphone: feed the audio system silence at the output rate, without taking one
            // of the console's audio output ports.
            audio_thread = std::jthread([this](std::stop_token stop_token) {
                std::array<s16, PREFERRED_SAMPLE_COUNT * MaxChannels> buffer{};
                while (!stop_token.stop_requested()) {
                    if (!paused) {
                        std::fill(buffer.begin(), buffer.end(), s16(0));
                        this->ProcessAudioIn(buffer, PREFERRED_SAMPLE_COUNT);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 * PREFERRED_SAMPLE_COUNT /
                                                                          TargetSampleRate));
                }
            });
            return;
        }
        auto const param = (device_channels > 1 ? ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO : ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_MONO);
        // The launcher's own sounds use an output port of the same kind: make sure it is closed.
        if (ps4_sound_close)
            ps4_sound_close();
        audio_dev = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, PREFERRED_SAMPLE_COUNT, TargetSampleRate, param);
        for (int attempt = 0; audio_dev <= 0 && attempt < 10; attempt++) { // a port still closing
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            audio_dev = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, PREFERRED_SAMPLE_COUNT, TargetSampleRate, param);
        }
        if (audio_dev <= 0) {
            // No port (the console refused one - e.g. a second stream while the first holds the
            // main port): still consume the game's audio at the output rate, silently. Without a
            // consumer nothing releases the game's audio buffers, and a game that waits for them
            // (a 3D game did) stops for good.
            LOG_ERROR(Service_Audio, "Failed to create audio device! {:#x} - this stream plays silently",
                      uint32_t(audio_dev));
            if (ps4_boot_trace)
                ps4_boot_trace("audio: no output port for a stream - it plays silently instead");
            audio_thread = std::jthread([this](std::stop_token stop_token) {
                std::array<s16, PREFERRED_SAMPLE_COUNT * MaxChannels> buffer;
                auto next = std::chrono::steady_clock::now();
                while (!stop_token.stop_requested()) {
                    std::fill(buffer.begin(), buffer.end(), s16(0));
                    if (!paused)
                        this->ProcessAudioOutAndRender(buffer, PREFERRED_SAMPLE_COUNT);
                    next += std::chrono::microseconds(1'000'000 * PREFERRED_SAMPLE_COUNT / TargetSampleRate);
                    std::this_thread::sleep_until(next);
                }
            });
            return;
        }
        if (audio_dev > 0) {
            audio_thread = std::jthread([=, this](std::stop_token stop_token) {
                std::array<s16, PREFERRED_SAMPLE_COUNT * MaxChannels> buffer;
                unsigned errors = 0;
                while (!stop_token.stop_requested()) {
                    std::fill(buffer.begin(), buffer.end(), s16(0));
                    if (!paused)
                        this->ProcessAudioOutAndRender(buffer, PREFERRED_SAMPLE_COUNT);
                    // Queues the block and returns once the port can take the next one, which
                    // paces this loop at the output rate. (Waiting for the previous block first,
                    // with a null output, left a gap after every block.) Silence while paused
                    // keeps the pacing without spinning a core.
                    if (const int err = sceAudioOutOutput(audio_dev, buffer.data()); err < 0 && errors++ < 8)
                        LOG_ERROR(Service_Audio, "sceAudioOutOutput: {:#x}", u32(err));
                }
                sceAudioOutOutput(audio_dev, nullptr); // let the last block finish
                sceAudioOutClose(audio_dev);
            });
        } else {
            LOG_ERROR(Service_Audio, "Failed to create audio device! {:#x}", uint32_t(audio_dev));
        }
    }

    ~PS4SinkStream() override {
        LOG_DEBUG(Service_Audio, "Destroying PS4 stream {}", name);
        if (audio_thread.joinable()) {
            audio_thread.request_stop();
            audio_thread.join();
        }
    }

    void Finalize() override {
        if (audio_dev > 0) {

        }
    }

    void Start(bool resume = false) override {
        if (audio_thread.joinable() && paused) {
            paused = false;
        }
    }

    void Stop() override {
        if (audio_thread.joinable() && !paused) {
            SignalPause();
        }
    }

    std::jthread audio_thread;
    int32_t audio_dev{};
};

PS4Sink::PS4Sink(std::string_view target_device_name) {
    int32_t rc = sceAudioOutInit();
    if (rc == 0 || unsigned(rc) == ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT) {
        if (target_device_name != auto_device_name && !target_device_name.empty()) {
            output_device = target_device_name;
        } else {
            output_device.clear();
        }
        device_channels = 2;
    } else {
        LOG_ERROR(Service_Audio, "Unable to open audio out! {:#x}", uint32_t(rc));
    }
}

PS4Sink::~PS4Sink() = default;

/// @brief Create a new sink stream.
/// @param system          - Core system.
/// @param system_channels - Number of channels the audio system expects. May differ from the device's channel count.
/// @param name            - Name of this stream.
/// @param type            - Type of this stream, render/in/out.
/// @return A pointer to the created SinkStream
SinkStream* PS4Sink::AcquireSinkStream(Core::System& system, u32 system_channels_, const std::string&, StreamType type) {
    system_channels = system_channels_;
    SinkStreamPtr& stream = sink_streams.emplace_back(std::make_unique<PS4SinkStream>(device_channels, system_channels, output_device, input_device, type, system));
    return stream.get();
}

void PS4Sink::CloseStream(SinkStream* stream) {
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams[i].reset();
            sink_streams.erase(sink_streams.begin() + i);
            break;
        }
    }
}

void PS4Sink::CloseStreams() {
    sink_streams.clear();
}

f32 PS4Sink::GetDeviceVolume() const {
    return sink_streams.size() > 0 ? sink_streams[0]->GetDeviceVolume() : 1.f;
}

void PS4Sink::SetDeviceVolume(f32 volume) {
    for (auto& stream : sink_streams)
        stream->SetDeviceVolume(volume);
}

void PS4Sink::SetSystemVolume(f32 volume) {
    for (auto& stream : sink_streams)
        stream->SetSystemVolume(volume);
}

std::vector<std::string> ListPS4SinkDevices(bool capture) {
    return {{"Default"}};
}

u32 GetPS4Latency() {
    return TargetSampleCount;
}

} // namespace AudioCore::Sink
