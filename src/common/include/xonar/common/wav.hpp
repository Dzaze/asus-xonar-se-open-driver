// Lecture et écriture de fichiers WAV (PCM 16/24/32 bits et float 32 bits),
// avec prise en charge de WAVE_FORMAT_EXTENSIBLE pour le masque de canaux.
// Usage : outils hors ligne et tests. Ne pas utiliser dans un chemin temps réel.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xonar::common {

enum class SampleFormat : std::uint8_t { Pcm16, Pcm24, Pcm32, Float32 };

struct WavInfo {
    std::uint32_t sampleRate = 48000;
    std::uint16_t channels = 2;
    std::uint32_t channelMask = 0;   // 0 = non spécifié (format WAVE_FORMAT_PCM/IEEE classique)
    SampleFormat format = SampleFormat::Float32;
    std::uint64_t frames = 0;
};

struct WavData {
    WavInfo info;
    std::vector<float> samples; // entrelacés, normalisés dans [-1, 1]
};

struct WavResult {
    bool ok = false;
    std::string error;
};

// Lit tout le fichier en mémoire et convertit en float32.
WavResult readWav(const std::string& path, WavData& out);

// Écrit les échantillons (entrelacés, float) au format demandé. Si channelMask != 0
// ou channels > 2, un en-tête WAVE_FORMAT_EXTENSIBLE est produit.
WavResult writeWav(const std::string& path, const WavInfo& info, const std::vector<float>& samples);

const char* sampleFormatName(SampleFormat f) noexcept;

} // namespace xonar::common
