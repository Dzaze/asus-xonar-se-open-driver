// dsp-demo : démonstrateur hors matériel. Lit un WAV, applique le pipeline dsp-core
// selon un mode choisi, écrit le résultat et imprime les statistiques (crêtes par
// canal, écrêtage, réduction du limiteur). Sert aussi à produire les fichiers
// "golden" de non-régression.
#include "xonar/common/wav.hpp"
#include "xonar/dsp/pipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace xonar;

namespace {

void usage() {
    std::printf(
        "dsp-demo : applique le moteur DSP Xonar SE Open Driver a un fichier WAV.\n\n"
        "Usage : dsp-demo --in <entree.wav> --out <sortie.wav> --mode <mode> [options]\n\n"
        "Modes :\n"
        "  passthrough          Copie les canaux tels quels (layout de sortie = entree).\n"
        "  upmix                Stereo -> 5.1 (sortie 5.1 side par defaut).\n"
        "  downmix              5.1 -> stereo (ITU-R BS.775, headroom -4.5 dB).\n"
        "  2.1                  Stereo ou 5.1 -> 2.1 virtuel sur sortie 5.1 : FL/FR + caisson passe-bas, reste muet.\n"
        "  test                 Genere un sinus sur un canal (--test-channel, --test-freq, --test-level).\n\n"
        "Options :\n"
        "  --output-mask <hex>  Masque de sortie (ex. 0x60F = 5.1 side, 0x3F = 5.1 back). Par defaut selon le mode.\n"
        "  --input-mask <hex>   Force le masque d'entree si le WAV n'en a pas.\n"
        "  --master-db <dB>     Gain maitre.\n"
        "  --gain <canal>=<dB>  Gain par canal (FL FR FC LFE BL BR SL SR), repetable.\n"
        "  --delay <canal>=<ms> Delai par canal, repetable.\n"
        "  --bass <Hz>          Active le bass management avec cette frequence de coupure.\n"
        "  --no-limiter         Desactive le limiteur (l'ecretage de securite reste compte).\n"
        "  --sub-hz <Hz>        Mode 2.1 : coupure du passe-bas du caisson (defaut 80).\n"
        "  --sub-db <dB>        Mode 2.1 : gain du caisson (defaut 0).\n"
        "  --block <trames>     Taille de bloc de traitement (defaut 480).\n"
        "  --format <f>         Format de sortie : float32 (defaut), pcm16, pcm24, pcm32.\n"
        "  --test-channel <c>   Canal du signal de test (defaut FL).\n"
        "  --test-freq <Hz>     Frequence du signal de test (defaut 440).\n"
        "  --test-level <dBFS>  Niveau du signal de test (defaut -20).\n"
        "  --duration <s>       Duree du signal de test si --in est omis (defaut 2).\n");
}

bool parseChannel(const std::string& s, dsp::Channel& out) {
    static const char* names[] = {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"};
    for (std::size_t i = 0; i < 8; ++i) {
        if (s == names[i]) { out = static_cast<dsp::Channel>(i); return true; }
    }
    return false;
}

std::uint32_t defaultOutputMask(dsp::Mode mode, std::uint32_t inputMask) {
    switch (mode) {
    case dsp::Mode::PassThrough: return inputMask;
    case dsp::Mode::Upmix2to6: return dsp::masks::k5Point1Side;
    case dsp::Mode::Downmix6to2: return dsp::masks::kStereo;
    case dsp::Mode::TwoPointOne: return dsp::masks::k5Point1Side;
    case dsp::Mode::ChannelTest: return dsp::masks::k5Point1Side;
    case dsp::Mode::Upmix6to6: return inputMask;          // 6->6 : layout préservé.
    case dsp::Mode::DownmixStereo6to6: return inputMask;  // 6->6 : layout préservé (downmix interne).
    }
    return inputMask;
}

std::uint32_t guessMask(std::uint16_t channels) {
    switch (channels) {
    case 1: return dsp::masks::kMono;
    case 2: return dsp::masks::kStereo;
    case 4: return dsp::masks::kQuad;
    case 6: return dsp::masks::k5Point1Side;
    case 8: return dsp::masks::k7Point1;
    default: return 0;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string inPath, outPath, modeName;
    std::uint32_t outputMask = 0, inputMaskOverride = 0, block = 480;
    double duration = 2.0;
    common::SampleFormat outFormat = common::SampleFormat::Float32;
    dsp::Profile overrides; // on ne lit que les champs renseignés par la ligne de commande
    bool bass = false, noLimiter = false;
    float bassHz = 80.f;
    float subHz = 80.f, subDb = 0.f;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& v) { if (i + 1 >= argc) return false; v = argv[++i]; return true; };
        std::string v;
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--in" && next(v)) inPath = v;
        else if (a == "--out" && next(v)) outPath = v;
        else if (a == "--mode" && next(v)) modeName = v;
        else if (a == "--output-mask" && next(v)) outputMask = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
        else if (a == "--input-mask" && next(v)) inputMaskOverride = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
        else if (a == "--master-db" && next(v)) overrides.masterGainDb = std::strtof(v.c_str(), nullptr);
        else if ((a == "--gain" || a == "--delay") && next(v)) {
            const auto eq = v.find('=');
            dsp::Channel c = dsp::Channel::FrontLeft;
            if (eq == std::string::npos || !parseChannel(v.substr(0, eq), c)) { std::fprintf(stderr, "Argument invalide : %s\n", v.c_str()); return 1; }
            const float val = std::strtof(v.c_str() + eq + 1, nullptr);
            if (a == "--gain") overrides.channelGainDb[static_cast<std::size_t>(c)] = val;
            else overrides.channelDelayMs[static_cast<std::size_t>(c)] = val;
        }
        else if (a == "--bass" && next(v)) { bass = true; bassHz = std::strtof(v.c_str(), nullptr); }
        else if (a == "--no-limiter") noLimiter = true;
        else if (a == "--sub-hz" && next(v)) subHz = std::strtof(v.c_str(), nullptr);
        else if (a == "--sub-db" && next(v)) subDb = std::strtof(v.c_str(), nullptr);
        else if (a == "--block" && next(v)) block = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (a == "--format" && next(v)) {
            if (v == "float32") outFormat = common::SampleFormat::Float32;
            else if (v == "pcm16") outFormat = common::SampleFormat::Pcm16;
            else if (v == "pcm24") outFormat = common::SampleFormat::Pcm24;
            else if (v == "pcm32") outFormat = common::SampleFormat::Pcm32;
            else { std::fprintf(stderr, "Format inconnu : %s\n", v.c_str()); return 1; }
        }
        else if (a == "--test-channel" && next(v)) { if (!parseChannel(v, overrides.test.channel)) { std::fprintf(stderr, "Canal inconnu : %s\n", v.c_str()); return 1; } }
        else if (a == "--test-freq" && next(v)) overrides.test.frequencyHz = std::strtof(v.c_str(), nullptr);
        else if (a == "--test-level" && next(v)) overrides.test.levelDb = std::strtof(v.c_str(), nullptr);
        else if (a == "--duration" && next(v)) duration = std::strtod(v.c_str(), nullptr);
        else { std::fprintf(stderr, "Option inconnue : %s\n", a.c_str()); usage(); return 1; }
    }

    dsp::Mode mode;
    if (modeName == "passthrough") mode = dsp::Mode::PassThrough;
    else if (modeName == "upmix") mode = dsp::Mode::Upmix2to6;
    else if (modeName == "downmix") mode = dsp::Mode::Downmix6to2;
    else if (modeName == "2.1") mode = dsp::Mode::TwoPointOne;
    else if (modeName == "test") mode = dsp::Mode::ChannelTest;
    else { std::fprintf(stderr, "Mode requis : passthrough | upmix | downmix | 2.1 | test\n"); return 1; }
    if (outPath.empty()) { std::fprintf(stderr, "--out est requis\n"); return 1; }
    if (block == 0) block = 480;

    // Entrée : fichier, ou silence stéréo pour le mode test.
    common::WavData input;
    if (!inPath.empty()) {
        const auto r = common::readWav(inPath, input);
        if (!r.ok) { std::fprintf(stderr, "Lecture impossible : %s\n", r.error.c_str()); return 1; }
    } else if (mode == dsp::Mode::ChannelTest) {
        input.info.sampleRate = 48000;
        input.info.channels = 2;
        input.info.channelMask = dsp::masks::kStereo;
        input.info.frames = static_cast<std::uint64_t>(duration * 48000);
        input.samples.assign(static_cast<std::size_t>(input.info.frames) * 2, 0.f);
    } else { std::fprintf(stderr, "--in est requis pour ce mode\n"); return 1; }

    std::uint32_t inputMask = inputMaskOverride ? inputMaskOverride
                            : (input.info.channelMask ? input.info.channelMask : guessMask(input.info.channels));
    const auto inLayout = dsp::ChannelLayout::fromMask(inputMask);
    if (inLayout.count != input.info.channels) {
        std::fprintf(stderr, "Le masque 0x%X (%u canaux) ne correspond pas au fichier (%u canaux). Utilisez --input-mask.\n",
                     inputMask, inLayout.count, input.info.channels);
        return 1;
    }
    if (!outputMask) outputMask = defaultOutputMask(mode, inputMask);

    dsp::Profile prof;
    prof.mode = mode;
    prof.sampleRate = input.info.sampleRate;
    prof.input = inLayout;
    prof.output = dsp::ChannelLayout::fromMask(outputMask);
    prof.masterGainDb = overrides.masterGainDb;
    prof.channelGainDb = overrides.channelGainDb;
    prof.channelDelayMs = overrides.channelDelayMs;
    prof.test = overrides.test;
    if (bass) { prof.bassManagement.enabled = true; prof.bassManagement.crossoverHz = bassHz; }
    prof.twoPointOne.lfeLowPassHz = subHz;
    prof.twoPointOne.lfeGainDb = subDb;
    if (noLimiter || mode == dsp::Mode::ChannelTest) prof.limiter.enabled = false;

    dsp::Pipeline pipeline;
    if (!pipeline.prepare(prof, block)) {
        std::fprintf(stderr, "Profil incoherent pour ce mode (entree 0x%X -> sortie 0x%X).\n", inputMask, outputMask);
        return 1;
    }

    const auto frames = static_cast<std::uint32_t>(input.info.frames);
    std::vector<float> output(static_cast<std::size_t>(frames) * prof.output.count, 0.f);
    for (std::uint32_t pos = 0; pos < frames; pos += block) {
        const std::uint32_t n = std::min(block, frames - pos);
        dsp::ConstInterleavedView in{input.samples.data() + static_cast<std::size_t>(pos) * prof.input.count, prof.input.count, n};
        dsp::InterleavedView out{output.data() + static_cast<std::size_t>(pos) * prof.output.count, prof.output.count, n};
        if (!pipeline.process(in, out)) { std::fprintf(stderr, "Erreur de traitement au bloc %u\n", pos); return 1; }
    }

    common::WavInfo outInfo;
    outInfo.sampleRate = prof.sampleRate;
    outInfo.channels = prof.output.count;
    outInfo.channelMask = prof.output.mask;
    outInfo.format = outFormat;
    const auto w = common::writeWav(outPath, outInfo, output);
    if (!w.ok) { std::fprintf(stderr, "Ecriture impossible : %s\n", w.error.c_str()); return 1; }

    const auto& st = pipeline.stats();
    std::printf("Entree  : %u canaux (masque 0x%X), %u Hz, %u trames, %s\n", prof.input.count, inputMask, prof.sampleRate, frames,
                common::sampleFormatName(input.info.format));
    std::printf("Sortie  : %u canaux (masque 0x%X), %s -> %s\n", prof.output.count, prof.output.mask,
                common::sampleFormatName(outFormat), outPath.c_str());
    std::printf("Cretes par canal :");
    for (std::uint32_t c = 0; c < prof.output.count; ++c)
        std::printf(" %s=%.1f dBFS", std::string(dsp::channelName(prof.output.order[c])).c_str(), dsp::linearToDb(st.outputPeak[c]));
    std::printf("\nLimiteur : reduction max %.1f dB ; echantillons ecretes : %llu\n", st.limiterMinGainDb,
                static_cast<unsigned long long>(st.clippedSamples));
    return 0;
}
