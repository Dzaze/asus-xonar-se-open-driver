#include "xonar/common/wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace xonar::common {

namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

// GUID des sous-formats : {00000001-0000-0010-8000-00AA00389B71} (PCM) et ...0003 (float).
constexpr std::array<std::uint8_t, 16> kSubFormatPcm = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
constexpr std::array<std::uint8_t, 16> kSubFormatFloat = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

std::uint16_t rd16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
void wr16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void wr32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}

std::uint16_t bitsPerSample(SampleFormat f) noexcept {
    switch (f) {
    case SampleFormat::Pcm16: return 16;
    case SampleFormat::Pcm24: return 24;
    case SampleFormat::Pcm32: return 32;
    case SampleFormat::Float32: return 32;
    }
    return 32;
}

float clampUnit(float x) noexcept { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); }

} // namespace

const char* sampleFormatName(SampleFormat f) noexcept {
    switch (f) {
    case SampleFormat::Pcm16: return "PCM 16 bits";
    case SampleFormat::Pcm24: return "PCM 24 bits";
    case SampleFormat::Pcm32: return "PCM 32 bits";
    case SampleFormat::Float32: return "IEEE float 32 bits";
    }
    return "?";
}

WavResult readWav(const std::string& path, WavData& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "impossible d'ouvrir le fichier : " + path};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return {false, "en-tête RIFF/WAVE invalide"};

    std::uint16_t formatTag = 0, channels = 0, bits = 0, validBits = 0;
    std::uint32_t sampleRate = 0, channelMask = 0;
    std::array<std::uint8_t, 16> subFormat{};
    bool haveFmt = false;
    const std::uint8_t* data = nullptr;
    std::uint32_t dataSize = 0;

    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::uint8_t* chunk = bytes.data() + pos;
        const std::uint32_t size = rd32(chunk + 4);
        const std::uint8_t* body = chunk + 8;
        const std::size_t avail = bytes.size() - (pos + 8);
        const std::uint32_t usable = static_cast<std::uint32_t>(std::min<std::size_t>(size, avail));

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (usable < 16) return {false, "chunk fmt trop court"};
            formatTag = rd16(body);
            channels = rd16(body + 2);
            sampleRate = rd32(body + 4);
            bits = rd16(body + 14);
            validBits = bits;
            if (formatTag == kFormatExtensible) {
                if (usable < 40) return {false, "chunk fmt extensible trop court"};
                validBits = rd16(body + 18);
                channelMask = rd32(body + 20);
                std::memcpy(subFormat.data(), body + 24, 16);
            }
            haveFmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = body;
            dataSize = usable;
        }
        pos += 8 + size + (size & 1u); // les chunks sont alignés sur 2 octets
    }

    if (!haveFmt || data == nullptr) return {false, "chunks fmt ou data absents"};
    if (channels == 0 || sampleRate == 0) return {false, "format incohérent"};

    bool isFloat = false;
    if (formatTag == kFormatIeeeFloat) isFloat = true;
    else if (formatTag == kFormatPcm) isFloat = false;
    else if (formatTag == kFormatExtensible) {
        if (subFormat == kSubFormatFloat) isFloat = true;
        else if (subFormat == kSubFormatPcm) isFloat = false;
        else return {false, "sous-format WAVE_FORMAT_EXTENSIBLE non pris en charge"};
    } else {
        return {false, "format WAV non pris en charge (tag " + std::to_string(formatTag) + ")"};
    }

    SampleFormat fmt;
    if (isFloat) {
        if (bits != 32) return {false, "float non 32 bits non pris en charge"};
        fmt = SampleFormat::Float32;
    } else {
        switch (bits) {
        case 16: fmt = SampleFormat::Pcm16; break;
        case 24: fmt = SampleFormat::Pcm24; break;
        case 32: fmt = SampleFormat::Pcm32; break;
        default: return {false, "profondeur PCM non prise en charge : " + std::to_string(bits)};
        }
    }

    const std::uint32_t bytesPerSample = bits / 8;
    const std::uint64_t frames = dataSize / (static_cast<std::uint64_t>(bytesPerSample) * channels);
    out.info.sampleRate = sampleRate;
    out.info.channels = channels;
    out.info.channelMask = channelMask;
    out.info.format = fmt;
    out.info.frames = frames;
    out.samples.resize(static_cast<std::size_t>(frames) * channels);

    const std::uint8_t* p = data;
    for (std::size_t i = 0; i < out.samples.size(); ++i, p += bytesPerSample) {
        float v = 0.f;
        switch (fmt) {
        case SampleFormat::Pcm16: {
            const auto s = static_cast<std::int16_t>(rd16(p));
            v = static_cast<float>(s) / 32768.f;
            break;
        }
        case SampleFormat::Pcm24: {
            std::int32_t s = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if (s & 0x800000) s |= static_cast<std::int32_t>(0xFF000000);
            v = static_cast<float>(s) / 8388608.f;
            break;
        }
        case SampleFormat::Pcm32: {
            const auto s = static_cast<std::int32_t>(rd32(p));
            v = static_cast<float>(static_cast<double>(s) / 2147483648.0);
            break;
        }
        case SampleFormat::Float32: {
            std::uint32_t u = rd32(p);
            std::memcpy(&v, &u, 4);
            break;
        }
        }
        out.samples[i] = v;
    }
    (void)validBits;
    return {true, {}};
}

WavResult writeWav(const std::string& path, const WavInfo& info, const std::vector<float>& samples) {
    if (info.channels == 0 || info.sampleRate == 0) return {false, "format incohérent"};
    if (samples.size() % info.channels != 0) return {false, "nombre d'échantillons non multiple du nombre de canaux"};

    const std::uint16_t bits = bitsPerSample(info.format);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(info.channels * bits / 8);
    const bool extensible = info.channelMask != 0 || info.channels > 2;
    const bool isFloat = info.format == SampleFormat::Float32;

    std::vector<std::uint8_t> body;
    body.reserve(64 + samples.size() * (bits / 8));

    // fmt
    body.insert(body.end(), {'f', 'm', 't', ' '});
    wr32(body, extensible ? 40u : 16u);
    wr16(body, extensible ? kFormatExtensible : (isFloat ? kFormatIeeeFloat : kFormatPcm));
    wr16(body, info.channels);
    wr32(body, info.sampleRate);
    wr32(body, info.sampleRate * blockAlign);
    wr16(body, blockAlign);
    wr16(body, bits);
    if (extensible) {
        wr16(body, 22);                 // cbSize
        wr16(body, bits);               // wValidBitsPerSample
        wr32(body, info.channelMask);   // dwChannelMask
        const auto& sub = isFloat ? kSubFormatFloat : kSubFormatPcm;
        body.insert(body.end(), sub.begin(), sub.end());
    }

    // data
    body.insert(body.end(), {'d', 'a', 't', 'a'});
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * (bits / 8));
    wr32(body, dataSize);
    for (float s : samples) {
        switch (info.format) {
        case SampleFormat::Pcm16: {
            const float c = clampUnit(s);
            const auto v = static_cast<std::int16_t>(std::lrint(c * 32767.f));
            wr16(body, static_cast<std::uint16_t>(v));
            break;
        }
        case SampleFormat::Pcm24: {
            const float c = clampUnit(s);
            const auto v = static_cast<std::int32_t>(std::lrint(c * 8388607.f));
            body.push_back(static_cast<std::uint8_t>(v & 0xFF));
            body.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
            body.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
            break;
        }
        case SampleFormat::Pcm32: {
            const double c = clampUnit(s);
            const auto v = static_cast<std::int32_t>(std::llrint(c * 2147483647.0));
            wr32(body, static_cast<std::uint32_t>(v));
            break;
        }
        case SampleFormat::Float32: {
            std::uint32_t u;
            std::memcpy(&u, &s, 4);
            wr32(body, u);
            break;
        }
        }
    }
    if (dataSize & 1u) body.push_back(0);

    std::vector<std::uint8_t> file;
    file.insert(file.end(), {'R', 'I', 'F', 'F'});
    wr32(file, static_cast<std::uint32_t>(4 + body.size()));
    file.insert(file.end(), {'W', 'A', 'V', 'E'});
    file.insert(file.end(), body.begin(), body.end());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "impossible de créer le fichier : " + path};
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!out) return {false, "erreur d'écriture : " + path};
    return {true, {}};
}

} // namespace xonar::common
