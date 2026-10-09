// SPDX-FileCopyrightText: Copyright 2026 PS4 Switch Emu
// SPDX-License-Identifier: GPL-3.0-or-later

// The app's own sounds (start screen, launcher, Home menu): synthesized on an audio port of their
// own, mixed from a few voices so they can overlap. The port is open only while menus show -
// ps4_sound_close() before a game starts its audio, and around the in-game Home menu.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>

#include "ps4_launcher.h"

extern "C" {
int32_t sceAudioOutInit();
int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t frames, uint32_t rate, uint32_t format);
int32_t sceAudioOutOutput(int32_t handle, const void* data);
int32_t sceAudioOutClose(int32_t handle);
}

namespace {

constexpr uint32_t kFrames = 256; // 5.3 ms at 48 kHz
constexpr float kRate = 48000.0f, kTau = 6.28318530718f;

// One note: a sine with a little of its octave (a softer, bell-like tone), a short attack (no
// click) and an exponential decay.
float Note(float t, float freq, float decay, float volume) {
    if (t < 0)
        return 0;
    const float attack = std::min(1.0f, t / 0.004f);
    const float env = attack * std::exp(-t / decay);
    return volume * env * (std::sin(kTau * freq * t) + 0.25f * std::sin(kTau * 2 * freq * t));
}

// A sound's sample at time t (seconds), and whether it has ended.
float Sample(int sound, float t, bool& done) {
    done = false;
    switch (sound) {
    case kSoundMove: // a soft tick
        done = t > 0.05f;
        return Note(t, 1760, 0.010f, 0.06f);
    case kSoundToggle: // a short pluck
        done = t > 0.09f;
        return Note(t, 1175, 0.025f, 0.08f);
    case kSoundConfirm: // two rising notes
        done = t > 0.25f;
        return Note(t, 880, 0.05f, 0.10f) + Note(t - 0.06f, 1320, 0.07f, 0.10f);
    case kSoundBack: // two falling notes
        done = t > 0.25f;
        return Note(t, 990, 0.05f, 0.08f) + Note(t - 0.06f, 660, 0.07f, 0.08f);
    case kSoundMenuOpen: // up, quick
        done = t > 0.3f;
        return Note(t, 660, 0.06f, 0.08f) + Note(t - 0.05f, 990, 0.09f, 0.08f);
    case kSoundMenuClose: // down, quick
        done = t > 0.3f;
        return Note(t, 990, 0.06f, 0.07f) + Note(t - 0.05f, 660, 0.09f, 0.07f);
    case kSoundError: // two low buzzes
        done = t > 0.32f;
        return 0.6f * (Note(t, 196, 0.06f, 0.10f) + Note(t - 0.14f, 196, 0.06f, 0.10f)) *
               (1 + 0.5f * std::sin(kTau * 392 * t));
    case kSoundStartup: { // the start screen: an E major arpeggio ringing out, then its chord
        done = t > 2.2f;
        const float notes[] = {659.25f, 830.61f, 987.77f, 1318.5f};
        float v = 0;
        for (int i = 0; i < 4; i++)
            v += Note(t - 0.11f * float(i), notes[i], 0.55f, 0.055f);
        for (int i = 0; i < 3; i++) // the chord, an octave down, swelling in softly
            v += 0.5f * std::min(1.0f, std::max(0.0f, (t - 0.45f) / 0.3f)) *
                 Note(t - 0.45f, notes[i] / 2, 0.9f, 0.05f);
        return v;
    }
    case kSoundLaunch: { // a game starts: a rising sweep into a chime
        done = t > 1.0f;
        float v = 0;
        if (t < 0.35f) {
            const float k = t / 0.35f;
            const float freq = 300 + 900 * k * k;
            v += 0.05f * std::sin(kTau * freq * t) * std::sin(k * 3.14159265f);
        }
        v += Note(t - 0.3f, 1318.5f, 0.35f, 0.08f) + Note(t - 0.3f, 1975.5f, 0.25f, 0.04f);
        return v;
    }
    default:
        done = true;
        return 0;
    }
}

class Mixer {
public:
    void Open() {
        std::scoped_lock lk{mutex};
        if (port > 0)
            return;
        (void)sceAudioOutInit(); // already initialised is fine
        port = sceAudioOutOpen(0xFF /* system user */, 0 /* main */, 0, kFrames, 48000, 1 /* S16 stereo */);
        if (port <= 0)
            return;
        stop = false;
        thread = std::thread([this] { Run(); });
    }

    // Waits (up to a second) for the sounds playing to end, then closes the port.
    void Close() {
        std::scoped_lock lk{mutex};
        if (port <= 0)
            return;
        for (int i = 0; i < 100 && busy.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stop = true;
        if (thread.joinable())
            thread.join();
        sceAudioOutClose(port);
        port = -1;
    }

    void Play(int sound) {
        for (auto& p : pending) {
            int expected = 0;
            if (p.compare_exchange_strong(expected, sound))
                return;
        }
    }

private:
    struct Voice {
        int sound = 0;
        uint32_t t = 0;
    };

    void Run() {
        int16_t block[kFrames * 2];
        Voice voices[4];
        while (!stop) {
            for (auto& p : pending) // new sounds take a free voice, or the oldest one
                if (const int s = p.exchange(0); s != 0) {
                    Voice* v = std::max_element(std::begin(voices), std::end(voices), [](const Voice& a, const Voice& b) {
                        return (a.sound == 0 ? UINT32_MAX : a.t) < (b.sound == 0 ? UINT32_MAX : b.t);
                    });
                    *v = Voice{s, 0};
                }
            bool any = false;
            for (uint32_t i = 0; i < kFrames; i++) {
                float mix = 0;
                for (auto& v : voices) {
                    if (v.sound == 0)
                        continue;
                    bool done = false;
                    mix += Sample(v.sound, float(v.t) / kRate, done);
                    v.t++;
                    if (done)
                        v.sound = 0;
                    else
                        any = true;
                }
                mix = std::tanh(mix * 1.5f) / 1.5f; // overlapping sounds never clip harshly
                block[i * 2] = block[i * 2 + 1] = int16_t(mix * 32767.0f);
            }
            busy = any;
            sceAudioOutOutput(port, block); // blocks until the port takes it: paces this loop
        }
        sceAudioOutOutput(port, nullptr);
    }

    std::mutex mutex;
    int32_t port = -1;
    std::thread thread;
    std::atomic<bool> stop{false}, busy{false};
    std::atomic<int> pending[4]{};
};

Mixer& GetMixer() {
    static Mixer mixer;
    return mixer;
}

} // namespace

extern "C" {

void ps4_sound_open() {
    GetMixer().Open();
}

void ps4_sound_play(int sound) {
    GetMixer().Play(sound);
}

void ps4_sound_close() {
    GetMixer().Close();
}

} // extern "C"
