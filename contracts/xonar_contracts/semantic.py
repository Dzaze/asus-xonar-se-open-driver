"""Règles sémantiques que JSON Schema n'exprime pas (manifeste : semanticRules).

Exécutées seulement sur un document déjà conforme à son schéma. Le contexte
optionnel porte les faits de la connexion négociée, jamais importés d'un
profil : sampleRate (Nyquist), nowQpc et qpcFrequency (expiration des tests).
"""

from __future__ import annotations

from typing import Any, Iterator

from .model import Manifest, Spec
from .normalize import normalize
from .schema_check import Issue, json_equal


def _walk(m: Manifest, struct_id: str, obj: Any, path: str) -> Iterator[tuple[str, str, dict[str, Any]]]:
    if not isinstance(obj, dict):
        return
    yield struct_id, path, obj
    st = m.structs[struct_id]
    for f in st.fields:
        if f.name not in obj or obj[f.name] is None:
            continue
        spec = f.spec
        if spec.kind == "switch":
            spec = spec.cases.get(obj.get(spec.switch_field))
            if spec is None:
                continue
        yield from _walk_spec(m, spec, obj[f.name], f"{path}/{f.name}")


def _walk_spec(m: Manifest, spec: Spec, v: Any, path: str) -> Iterator[tuple[str, str, dict[str, Any]]]:
    if spec.kind == "struct":
        yield from _walk(m, spec.ref, v, path)
    elif spec.kind == "union":
        u = m.unions[spec.ref]
        for member in u.members:
            const = m.structs[member].field(u.discriminator).spec.constraints["const"]
            if isinstance(v, dict) and v.get(u.discriminator) == const:
                yield from _walk(m, member, v, path)
    elif spec.kind == "array" and isinstance(v, list):
        for i, x in enumerate(v):
            yield from _walk_spec(m, spec.item, x, f"{path}/{i}")


def _instances_rules(issues: list[Issue], path: str, committed: int, desired: int,
                     instances: list[dict[str, Any]], key: str) -> None:
    if committed > desired:
        issues.append(Issue("semantic.generationOrder", path + "/committedGeneration",
                            f"committedGeneration {committed} > desiredGeneration {desired}"))
    seen: set[str] = set()
    for i, inst in enumerate(instances):
        if inst["appliedGeneration"] > committed:
            issues.append(Issue("semantic.appliedAheadOfCommitted", f"{path}/{key}/{i}/appliedGeneration",
                                f"{inst['appliedGeneration']} > committedGeneration {committed}"))
        if inst["instanceId"] in seen:
            issues.append(Issue("semantic.duplicateInstance", f"{path}/{key}/{i}/instanceId",
                                f"instance « {inst['instanceId']} » dupliquée"))
        seen.add(inst["instanceId"])


def check(m: Manifest, struct_id: str, doc: Any, context: dict[str, Any] | None = None) -> list[Issue]:
    ctx = context or {}
    issues: list[Issue] = []
    sr = ctx.get("sampleRate")
    if sr is not None:
        lo, hi = m.const("DspSampleRateMin"), m.const("DspSampleRateMax")
        if isinstance(sr, bool) or not isinstance(sr, int) or not lo <= sr <= hi:
            issues.append(Issue("semantic.sampleRateRange", "", f"fréquence de contexte {sr} hors de {lo}…{hi} Hz"))
            sr = None
    lists = {
        "ListeningMode": m.enums["ListeningMode"],
    }
    for sid, path, obj in _walk(m, struct_id, doc, ""):
        st = m.structs[sid]
        if sr is not None:
            nyq = sr / 2
            for f in st.fields:
                if f.nyquist and f.name in obj and isinstance(obj[f.name], (int, float)) and obj[f.name] >= nyq:
                    issues.append(Issue("semantic.nyquist", f"{path}/{f.name}",
                                        f"{obj[f.name]} Hz ≥ Nyquist {nyq:g} Hz à {sr} Hz"))
        if sid == "OutputSettings":
            max_delay = obj.get("maxDelayMs", st.field("maxDelayMs").default)
            for i, d in enumerate(obj.get("channelDelayMs", [])):
                if d > max_delay:
                    issues.append(Issue("semantic.delayExceedsMax", f"{path}/channelDelayMs/{i}",
                                        f"délai {d} ms > maxDelayMs {max_delay} ms"))
        elif sid == "CapxDocument":
            if not json_equal(normalize(m, "ProfileDocument", obj["payload"]), obj["payload"]):
                issues.append(Issue("semantic.capxPayloadNotNormalized", f"{path}/payload",
                                    "la charge utile CAPX doit être un document complet normalisé"))
        elif sid == "NegotiatedFormat":
            bits = bin(obj["channelMask"]).count("1")
            if bits != obj["channelCount"]:
                issues.append(Issue("semantic.maskChannelCount", f"{path}/channelCount",
                                    f"{obj['channelCount']} canaux pour un masque de {bits} bits"))
        elif sid == "ControlResponse":
            _instances_rules(issues, path, obj["committedGeneration"], obj["desiredGeneration"],
                             obj["appliedInstances"], "appliedInstances")
            if obj["status"] == "Applied":
                inst = obj["appliedInstances"]
                if not inst or any(x["appliedGeneration"] != obj["committedGeneration"] for x in inst):
                    issues.append(Issue("semantic.appliedStatusMismatch", f"{path}/status",
                                        "Applied exige que toutes les instances aient appliqué committedGeneration"))
        elif sid == "AgentState":
            committed = obj["committedGeneration"]
            _instances_rules(issues, path, committed, obj["desiredGeneration"], obj["instances"], "instances")
            diverged = any(x["appliedGeneration"] != committed for x in obj["instances"])
            if obj["partiallyApplied"] != diverged:
                issues.append(Issue("semantic.partiallyAppliedMismatch", f"{path}/partiallyApplied",
                                    f"partiallyApplied={obj['partiallyApplied']} alors que divergence={diverged}"))
            mode, req, res = obj["listeningMode"], obj["requestedPolicy"], obj["resolvedPolicy"]
            if mode is None or req is None:
                allowed = {"conservative"}
            elif lists["ListeningMode"].by_wire(mode).attrs["neutralRouting"]:
                allowed = {"neutral", "conservative"}
            else:
                allowed = {req, "conservative"}
            if res not in allowed:
                issues.append(Issue("semantic.resolvedPolicy", f"{path}/resolvedPolicy",
                                    f"« {res} » incompatible avec mode={mode}, demande={req} ; attendu {sorted(allowed)}"))
            meters = obj["meters"]
            if meters is not None:
                stale_ms = m.const("TelemetryStaleMs")
                if meters["stale"] != (meters["ageMs"] > stale_ms):
                    issues.append(Issue("semantic.staleMeters", f"{path}/meters/stale",
                                        f"ageMs={meters['ageMs']} et stale={meters['stale']} (seuil {stale_ms} ms)"))
        elif sid == "ChannelTestCommand":
            now, freq = ctx.get("nowQpc"), ctx.get("qpcFrequency")
            if now is not None and freq is not None:
                limit = now + freq * m.const("ChannelTestMaxExpiryMs") // 1000
                if not now < obj["expiresQpc"] <= limit:
                    issues.append(Issue("semantic.channelTestExpiry", f"{path}/expiresQpc",
                                        f"expiresQpc {obj['expiresQpc']} hors de ]{now}, {limit}]"))
    return issues


def check_endpoint_sample_rate(m: Manifest, sample_rate: int) -> list[Issue]:
    """Validation d'endpoint, distincte de la validation DSP (C3)."""
    rates = m.const("XonarRenderSampleRates")
    if sample_rate not in rates:
        return [Issue("endpoint.sampleRateUnsupported", "", f"{sample_rate} Hz non annoncé par le rendu Xonar {rates}")]
    return []
