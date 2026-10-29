// Test de conformité C++ des contrats v1 (E0-02).
// Vérifie, sans dépendance hors en-têtes, que l'en-tête généré depuis
// contracts/manifest.json reste aligné sur dsp-core (énumérations, masques,
// défauts du Profile) et que ses tables filaires et GUID sont cohérents.
#include "xonar/contracts/v1/contracts.hpp"
#include "xonar/dsp/profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

namespace c = ::xonar::contracts::v1;
namespace dsp = ::xonar::dsp;

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("ÉCHEC : %s\n", label);
    }
}

#define XONAR_CHECK(cond, label) report((cond), (label))

#include "dsp_conformance.g.inc"

template <typename E, std::size_t N>
void checkWireRoundTrip(const std::array<E, N>& all, const char* label) {
    for (const E v : all) {
        const std::string_view w = c::toWire(v);
        E back{};
        report(!w.empty() && c::fromWire(w, back) && back == v, label);
    }
    E dummy{};
    report(!c::fromWire("valeur-inconnue", dummy), label);
}

constexpr std::uint32_t physicalOrderMask() {
    std::uint32_t m = 0;
    for (const c::Channel ch : c::kPhysicalOutputOrder) m |= c::maskBitOf(ch);
    return m;
}

static_assert(physicalOrderMask() == c::kPhysicalOutputMask, "ordre physique ≠ 0x60F");
static_assert(c::kPhysicalOutputOrder.size() == c::kPhysicalOutputChannels);
static_assert(c::kClsidSfxApo != c::kClsidEfxApo);
static_assert(c::kCapxContextSfx != c::kCapxContextEfx);
static_assert(c::kPkeySfxProfileDocument.fmtid == c::kCapxPropertySet);
static_assert(c::kPkeySfxProfileDocument != c::kPkeyEfxProfileDocument);
static_assert(c::efxDspModeOf(c::ListeningMode::Stereo20) == c::DspMode::Downmix6to2);
static_assert(c::efxDspModeOf(c::ListeningMode::TwoPointOne) == c::DspMode::TwoPointOne);
static_assert(c::neutralRoutingOf(c::ListeningMode::Stereo20) && !c::neutralRoutingOf(c::ListeningMode::Upmix));
static_assert(c::accessOf(c::Command::ApplyProfile) == c::CommandAccess::Write);
static_assert(c::scopeOf(c::Command::GetCapabilities) == c::CommandScope::Agent);
static_assert(c::kContractVersions.size() == 8 && c::kC3Major == 1);
static_assert(c::kMaxDocumentBytes == 128u * 1024u && c::kMaxDocumentDepth == 16u);
// La borne haute de l'EQ reste sous Nyquist de la plus basse fréquence Xonar.
static_assert(c::limits::EqBand::frequencyHzMax < c::kXonarRenderSampleRates[0] / 2.0);

} // namespace

int main() {
    checkDspDefaults();

    checkWireRoundTrip(c::kAllChannel, "Channel");
    checkWireRoundTrip(c::kAllDspMode, "DspMode");
    checkWireRoundTrip(c::kAllListeningMode, "ListeningMode");
    checkWireRoundTrip(c::kAllSourcePolicy, "SourcePolicy");
    checkWireRoundTrip(c::kAllResolvedPolicy, "ResolvedPolicy");
    checkWireRoundTrip(c::kAllCommand, "Command");
    checkWireRoundTrip(c::kAllErrorCode, "ErrorCode");
    checkWireRoundTrip(c::kAllResponseStatus, "ResponseStatus");

    for (std::size_t i = 0; i < c::kAllChannel.size(); ++i) {
        const dsp::Channel ch = static_cast<dsp::Channel>(i);
        report(c::toWire(c::kAllChannel[i]) == dsp::channelName(ch), "Channel : nom filaire = channelName");
    }

    // Les GUID textuels sont au format {8-4-4-4-12}.
    for (const std::string_view g : {c::kClsidSfxApoString, c::kClsidEfxApoString, c::kCapxPropertySetString,
                                     c::kCapxContextSfxString, c::kCapxContextEfxString}) {
        report(g.size() == 38 && g.front() == '{' && g.back() == '}', "GUID textuel");
    }

    // Défauts structurels des DTO.
    const c::ControlRequest req{};
    report(req.apiMajor == c::kApiMajor && std::holds_alternative<c::EmptyPayload>(req.payload),
           "ControlRequest par défaut");
    const c::CapxDocument capx{};
    report(capx.contractMajor == c::kCapxContractMajor && capx.payload.schemaVersion == c::kProfileSchemaVersion,
           "CapxDocument par défaut");
    const c::VolumeLevelControl vol{};
    report(vol.control == c::HardwareControl::Volume, "discriminant VolumeLevelControl");
    const c::EndpointCapability cap{};
    report(cap.physicalChannelMask == c::kPhysicalOutputMask, "physicalChannelMask");

    std::printf("%d vérifications, %d échec(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
