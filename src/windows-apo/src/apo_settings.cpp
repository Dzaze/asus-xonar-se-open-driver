// SPDX-License-Identifier: Apache-2.0
#include "apo_settings.hpp"

#include "xonar/dsp/channels.hpp"

#include <windows.h>
#include <shlobj.h>
#include <TraceLoggingProvider.h>

#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <mutex>

namespace xonar::apo {
TRACELOGGING_DEFINE_PROVIDER(g_apoTrace, "XonarSE.Apo.Diagnostics",
    (0x6a636141,0xb93c,0x40c6,0xb9,0x34,0x4a,0x4c,0x08,0x48,0xfb,0x29));
namespace {
INIT_ONCE g_traceInit = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK registerTrace(PINIT_ONCE, PVOID, PVOID*) {
    return TraceLoggingRegister(g_apoTrace) == ERROR_SUCCESS;
}
}
void apoTraceUnregister() noexcept { TraceLoggingUnregister(g_apoTrace); }
namespace {

// Le prototype est partagé avec LOCAL SERVICE : le déploiement prépare ses ACL.
// Ne dépend pas de l'environnement du compte interactif dans audiodg.
std::wstring configDir() {
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &known))) {
        std::wstring dir(known);
        CoTaskMemFree(known);
        dir += L"\\XonarSE";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(buf, n);
    dir += L"\\XonarSE";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring settingsPath(Stage stage) {
    const std::wstring dir = configDir();
    if (dir.empty()) {
        return {};
    }
    (void)stage;
    return dir + L"\\apo-efx.cfg"; // contrôle commun ; calibration appliquée par l'EFX seul
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool parseBool(const std::string& v, bool fallback) {
    if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return fallback;
}

float parseFloat(const std::string& v, float fallback) {
    try {
        std::size_t consumed = 0;
        const float value = std::stof(v, &consumed);
        return consumed == v.size() && std::isfinite(value) ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

// Index xonar::dsp::Channel d'un suffixe de clé (chGainDb_FL, muteSR, ...).
int channelIndex(const std::string& name) {
    using xonar::dsp::Channel;
    if (name == "FL") return static_cast<int>(Channel::FrontLeft);
    if (name == "FR") return static_cast<int>(Channel::FrontRight);
    if (name == "FC") return static_cast<int>(Channel::FrontCenter);
    if (name == "LFE") return static_cast<int>(Channel::LowFrequency);
    if (name == "BL") return static_cast<int>(Channel::BackLeft);
    if (name == "BR") return static_cast<int>(Channel::BackRight);
    if (name == "SL") return static_cast<int>(Channel::SideLeft);
    if (name == "SR") return static_cast<int>(Channel::SideRight);
    return -1;
}

// Normalise la valeur de la clé 'mode' (noms de ListeningMode du contrat :
// "5.1", "upmix", "2.1", "2.0"). Valeur inconnue -> repli (clé ignorée).
ApoSettings::EfxMode parseEfxMode(const std::string& v, ApoSettings::EfxMode fallback) {
    if (v == "5.1") return ApoSettings::EfxMode::Surround51;
    if (v == "upmix") return ApoSettings::EfxMode::Upmix;
    if (v == "2.1") return ApoSettings::EfxMode::TwoPointOne;
    if (v == "2.0") return ApoSettings::EfxMode::Stereo20;
    return fallback;
}

// Découpe "freq,q,gain" (culture invariante, point décimal) en trois flottants.
// Retourne false si le format est invalide (la bande reste alors inchangée).
bool parseEqTriplet(const std::string& v, float& freq, float& q, float& gain) {
    const auto c1 = v.find(',');
    if (c1 == std::string::npos) return false;
    const auto c2 = v.find(',', c1 + 1);
    if (c2 == std::string::npos) return false;
    try {
        freq = std::stof(trim(v.substr(0, c1)));
        q = std::stof(trim(v.substr(c1 + 1, c2 - c1 - 1)));
        gain = std::stof(trim(v.substr(c2 + 1)));
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

const char* stageName(Stage s) noexcept { return s == Stage::Efx ? "EFX" : "SFX"; }

ApoSettings loadApoSettings(Stage stage) noexcept {
    ApoSettings s{};
    try {
        const std::wstring path = settingsPath(stage);
        if (path.empty()) {
            return s;
        }
        // Autoriser le remplacement atomique pendant la lecture : ifstream/CRT
        // ne partage pas toujours DELETE et pouvait faire échouer l'écriture UI.
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if(handle == INVALID_HANDLE_VALUE) return s;
        struct ScopedRead { HANDLE h; ~ScopedRead() { CloseHandle(h); } } file{handle};
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || size.QuadPart > 128 * 1024) return s;
        std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD bytes = 0;
        if(!ReadFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &bytes, nullptr) || bytes != contents.size()) return s;
        std::istringstream in(contents);
        std::string line;
        while (std::getline(in, line)) {
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#') {
                continue;
            }
            const auto eq = t.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = trim(t.substr(0, eq));
            const std::string val = trim(t.substr(eq + 1));

            if (key == "enabled") {
                s.enabled = parseBool(val, s.enabled);
            } else if (key == "masterGainDb") {
                s.masterGainDb = parseFloat(val, s.masterGainDb);
            } else if (key == "limiterEnabled") {
                s.limiterEnabled = parseBool(val, s.limiterEnabled);
            } else if (key == "limiterCeilingDb") {
                s.limiterCeilingDb = parseFloat(val, s.limiterCeilingDb);
            } else if (key == "bassEnabled") {
                s.bassEnabled = parseBool(val, s.bassEnabled);
            } else if (key == "crossoverHz") {
                s.crossoverHz = parseFloat(val, s.crossoverHz);
            } else if (key == "sfxUpmixStereo") {
                s.sfxUpmixStereo = parseBool(val, s.sfxUpmixStereo);
            } else if (key == "sourcePolicy") {
                s.sfxUpmixStereo = val == "autoDeclaredLayout";
            } else if (key == "streamUpmix") {
                s.streamUpmix = parseBool(val, false);
            } else if (key == "upmix") {
                s.upmix = parseBool(val, s.upmix);
            } else if (key.rfind("chGainDb_", 0) == 0) {
                const int idx = channelIndex(key.substr(9));
                if (idx >= 0) {
                    s.channelGainDb[idx] = parseFloat(val, s.channelGainDb[idx]);
                }
            } else if (key.rfind("mute", 0) == 0) {
                const int idx = channelIndex(key.substr(4));
                if (idx >= 0) {
                    s.channelMute[idx] = parseBool(val, s.channelMute[idx]);
                }
            }
            // --- Extensions du format .cfg (étage EFX) ---
            else if (key == "mode") {
                s.mode = parseEfxMode(val, s.mode);
            } else if (key == "limiterReleaseMs") {
                s.limiterReleaseMs = parseFloat(val, s.limiterReleaseMs);
            } else if (key == "bassHighPass") {
                s.bassHighPass = parseBool(val, s.bassHighPass);
            } else if (key == "bassSumToLfe") {
                s.bassSumToLfe = parseBool(val, s.bassSumToLfe);
            } else if (key == "bassSumToLfeGainDb") {
                s.bassSumToLfeGainDb = parseFloat(val, s.bassSumToLfeGainDb);
            } else if (key == "upmixCenterDb") {
                s.upmixCenterDb = parseFloat(val, s.upmixCenterDb);
            } else if (key == "upmixSurroundDb") {
                s.upmixSurroundDb = parseFloat(val, s.upmixSurroundDb);
            } else if (key == "upmixLfeDb") {
                s.upmixLfeDb = parseFloat(val, s.upmixLfeDb);
            } else if (key == "upmixLfeHz") {
                s.upmixLfeHz = parseFloat(val, s.upmixLfeHz);
            } else if (key == "upmixDeriveLfe") {
                s.upmixDeriveLfe = parseBool(val, s.upmixDeriveLfe);
            } else if (key.rfind("chDelayMs_", 0) == 0) {
                const int idx = channelIndex(key.substr(10));
                if (idx >= 0) {
                    float d = parseFloat(val, s.channelDelayMs[idx]);
                    if (d < 0.f) d = 0.f;
                    if (d > 50.f) d = 50.f; // capacité des lignes à retard (maxDelayMs)
                    s.channelDelayMs[idx] = d;
                }
            } else if (key.rfind("eq_", 0) == 0) {
                // eq_<ch>_<b>=freq,q,gain : une ligne par bande ACTIVE ; présence = activée.
                const std::string rest = key.substr(3); // "<ch>_<b>"
                const auto us = rest.rfind('_');
                if (us != std::string::npos) {
                    const int idx = channelIndex(rest.substr(0, us));
                    int band = -1;
                    try {
                        band = std::stoi(rest.substr(us + 1));
                    } catch (...) {
                        band = -1;
                    }
                    if (idx >= 0 && band >= 0 && band < static_cast<int>(xonar::dsp::kEqBandsPerChannel)) {
                        float f = s.eq[idx][band].frequencyHz;
                        float q = s.eq[idx][band].q;
                        float g = s.eq[idx][band].gainDb;
                        if (parseEqTriplet(val, f, q, g)) {
                            s.eq[idx][band].enabled = true;
                            s.eq[idx][band].frequencyHz = f;
                            s.eq[idx][band].q = q;
                            s.eq[idx][band].gainDb = g;
                        }
                    }
                }
            }
        }
    } catch (...) {
        return ApoSettings{}; // toute erreur -> identité sûre
    }
    return s;
}

xonar::dsp::Profile buildProfile(Stage stage, std::uint32_t inputMask, std::uint32_t outputMask,
                                 std::uint32_t sampleRate, const ApoSettings& s) noexcept {
    using namespace xonar::dsp;
    Profile p{};
    p.sampleRate = sampleRate;
    p.maxDelayMs = 50.f;
    p.gainRampMs = 20;

    if (stage == Stage::Efx) {
        // Après mixage : 6 -> 6. Le mode d'écoute vient de la clé 'mode' (prioritaire) ;
        // la clé booléenne 'upmix' sert de repli de compatibilité quand 'mode' est absente.
        p.input = ChannelLayout::fromMask(outputMask);
        p.output = ChannelLayout::fromMask(outputMask);
        // Garde-fou : sans sortie 5.1 (6 canaux), aucun mode 6->6 n'est applicable -> PassThrough.
        if (p.input.is5Point1()) {
            switch (s.mode) {
            case ApoSettings::EfxMode::Surround51:  p.mode = Mode::PassThrough; break;
            case ApoSettings::EfxMode::Upmix:       p.mode = s.streamUpmix ? Mode::PassThrough : Mode::Upmix6to6; break;
            case ApoSettings::EfxMode::TwoPointOne: p.mode = Mode::TwoPointOne; break;
            case ApoSettings::EfxMode::Stereo20:    p.mode = Mode::DownmixStereo6to6; break;
            case ApoSettings::EfxMode::Unset:
                // Compat : 'upmix=1' -> Upmix6to6, sinon PassThrough (comportement actuel).
                p.mode = s.upmix ? Mode::Upmix6to6 : Mode::PassThrough;
                break;
            }
        } else {
            p.mode = Mode::PassThrough;
        }
    } else {
        // Par flux, sortie toujours 6 canaux / 0x60F.
        p.output = ChannelLayout::fromMask(outputMask);
        if (inputMask == masks::kStereo || inputMask == speakerMaskBit(Channel::FrontCenter) || inputMask == speakerMaskBit(Channel::FrontLeft)) {
            p.input = ChannelLayout::fromMask(inputMask);
            p.mode = p.input.count == 1 ? Mode::Upmix1to6 : Mode::Upmix2to6;
            if (!s.sfxUpmixStereo && !s.streamUpmix) {
                // Extension neutre : FL/FR conservés, le reste muet (pas d'upmix).
                p.channelMute[static_cast<int>(Channel::FrontCenter)] = true;
                p.channelMute[static_cast<int>(Channel::LowFrequency)] = true;
                p.channelMute[static_cast<int>(Channel::SideLeft)] = true;
                p.channelMute[static_cast<int>(Channel::SideRight)] = true;
                p.channelMute[static_cast<int>(Channel::BackLeft)] = true;
                p.channelMute[static_cast<int>(Channel::BackRight)] = true;
            }
        } else {
            // Entrée 6 canaux (0x3F ou 0x60F) : préservée positionnellement.
            p.mode = s.streamUpmix ? Mode::Upmix6to6 : Mode::PassThrough;
            p.input = ChannelLayout::fromMask(masks::k5Point1Side);
            p.output = ChannelLayout::fromMask(masks::k5Point1Side);
        }
    }

    // LFX/SFX : routage par flux uniquement. Gains, EQ, délais, bass management et
    // protection appartiennent au GFX/EFX pour ne jamais appliquer deux calibrations.
    if(stage == Stage::Sfx) {
        p.maxDelayMs = 0; // les lignes de retard complètes restent au seul étage GFX
        p.limiter.enabled = false;
        p.outputProtection = false;
        p.upmix.centerGainDb = s.upmixCenterDb;
        p.upmix.surroundGainDb = s.upmixSurroundDb;
        p.upmix.lfeGainDb = s.upmixLfeDb;
        p.upmix.lfeLowPassHz = s.upmixLfeHz;
        p.upmix.deriveLfe = s.upmixDeriveLfe;
        return p;
    }

    // Compensation de sortie unique.
    p.upmixHandledPerStream = s.streamUpmix;
    p.autoDeclaredLayout = s.sfxUpmixStereo && s.mode == ApoSettings::EfxMode::Surround51;
    p.masterGainDb = s.masterGainDb;
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        p.channelGainDb[i] = s.channelGainDb[i];
        p.channelMute[i] = p.channelMute[i] || s.channelMute[i];
    }
    p.limiter.enabled = s.limiterEnabled;
    p.limiter.ceilingDb = s.limiterCeilingDb;
    p.limiter.releaseMs = s.limiterReleaseMs;
    p.bassManagement.enabled = s.bassEnabled;
    p.bassManagement.crossoverHz = s.crossoverHz;
    p.bassManagement.highPassSatellites = s.bassHighPass;
    p.bassManagement.sumToLfe = s.bassSumToLfe;
    p.bassManagement.sumToLfeGainDb = s.bassSumToLfeGainDb;

    // Délais par canal et égaliseur (indexés par valeur de Channel ; défauts = identité).
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        p.channelDelayMs[i] = s.channelDelayMs[i];
        for (std::size_t b = 0; b < kEqBandsPerChannel; ++b) {
            p.eq[i][b] = s.eq[i][b];
        }
    }

    // Sous-réglages de l'upmix (utiles pour Upmix2to6 / Upmix6to6 ; inertes sinon).
    p.upmix.centerGainDb = s.upmixCenterDb;
    p.upmix.surroundGainDb = s.upmixSurroundDb;
    p.upmix.lfeGainDb = s.upmixLfeDb;
    p.upmix.lfeLowPassHz = s.upmixLfeHz;
    p.upmix.deriveLfe = s.upmixDeriveLfe;
    p.twoPointOne.lfeLowPassHz = s.crossoverHz;
    return p;
}

void writeApoStatus(Stage stage, int instanceId, std::uint64_t fileTime,
                    const xonar::dsp::Profile& profile, const float* inputRms,
                    const float* outputRms, const float* correlation, bool active,
                    const RuntimeMeters& meters) noexcept {
    try {
        // Appels exclusivement hors callback : instantané des flux du même audiodg.
        struct Source { std::string json; ULONGLONG seen; };
        static std::mutex gate;
        static std::map<int, Source> sources;
        std::string sourceJson;
        {
            const std::lock_guard lock(gate);
            const auto now = GetTickCount64();
            for(auto it = sources.begin(); it != sources.end();)
                if(now - it->second.seen > 1500) it = sources.erase(it); else ++it;
            if(stage == Stage::Sfx) {
                if(!active) { sources.erase(instanceId); return; }
                std::ostringstream s;
                s << "{\"instance\":" << instanceId << ",\"inputChannels\":" << unsigned(profile.input.count)
                  << ",\"inputMask\":" << profile.input.mask << ",\"sampleRate\":" << profile.sampleRate
                  << ",\"configFileTime\":" << fileTime << ",\"enabled\":" << (meters.enabled ? "true" : "false") << '}';
                if(sources.size() < 64 || sources.contains(instanceId)) sources[instanceId] = {s.str(), now};
                return;
            }
            for(const auto& [id, value] : sources) {
                if(!sourceJson.empty()) sourceJson += ',';
                sourceJson += value.json;
            }
        }
        const auto path = configDir() + L"\\apo-live-status.json";
        std::ostringstream json;
        json << "{\"configFileTime\":" << fileTime << ",\"pid\":" << GetCurrentProcessId()
             << ",\"instance\":" << instanceId << ",\"mode\":" << static_cast<unsigned>(profile.upmixHandledPerStream ? xonar::dsp::Mode::Upmix6to6 : profile.mode)
             << ",\"processingMode\":" << static_cast<unsigned>(profile.mode)
             << ",\"auto\":" << (profile.autoDeclaredLayout ? "true" : "false")
             << ",\"channels\":" << static_cast<unsigned>(profile.output.count) << ",\"sampleRate\":" << profile.sampleRate
             << ",\"inputMask\":" << profile.input.mask << ",\"outputMask\":" << profile.output.mask
             << ",\"enabled\":" << (meters.enabled ? "true" : "false")
             << ",\"preLimiterPeak\":" << meters.preLimiterPeak
             << ",\"clippedSamples\":" << meters.clippedSamples
             << ",\"limiterReductionDb\":" << meters.limiterReductionDb
             << ",\"sources\":[" << sourceJson << "],\"outputPeak\":[";
        for(std::uint32_t c = 0; c < profile.output.count; ++c) { if(c) json << ','; json << meters.peaks[c]; }
        json << ']'
             << ",\"active\":" << (active ? "true" : "false") << ",\"inputRms\":[";
        for (std::uint32_t c = 0; c < profile.input.count; ++c) {
            if (c) json << ',';
            json << (inputRms ? inputRms[c] : 0.f);
        }
        json << "],\"outputRms\":[";
        for (std::uint32_t c = 0; c < profile.output.count; ++c) {
            if (c) json << ',';
            json << (outputRms ? outputRms[c] : 0.f);
        }
        json << "],\"correlation\":[";
        for (std::uint32_t c = 0; c < profile.output.count; ++c) {
            if (c) json << ',';
            json << (correlation ? correlation[c] : 0.f);
        }
        json << "]}";
        // Le déploiement prépare ce fichier, avec Modify pour LOCAL SERVICE.
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const auto text = json.str(); DWORD written = 0;
        if (WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size())
            SetEndOfFile(file);
        CloseHandle(file);
    } catch (...) {}
}

void apoLog(Stage stage, int instanceId, const std::string& line) noexcept {
    try {
        InitOnceExecuteOnce(&g_traceInit, registerTrace, nullptr, nullptr);
        TraceLoggingWrite(g_apoTrace, "Lifecycle", TraceLoggingString(stageName(stage), "Stage"),
                          TraceLoggingInt32(instanceId, "Instance"), TraceLoggingString(line.c_str(), "Message"));
        const std::wstring dir = configDir();
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[32];
        std::snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::ostringstream record;
        record << ts << " pid=" << GetCurrentProcessId() << " [" << stageName(stage)
               << '#' << instanceId << "] " << line << '\n';
        const std::string text = record.str();
        if (!dir.empty()) {
            const std::wstring path = dir + L"\\apo-log.txt";
            HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                CloseHandle(file);
                if (ok && written == text.size()) return;
            }
        }
        // Repli observable par un débogueur même si le répertoire/ACL manque.
        OutputDebugStringA(text.c_str());
    } catch (...) {
        // Le journal ne doit jamais perturber l'audio.
    }
}

} // namespace xonar::apo
