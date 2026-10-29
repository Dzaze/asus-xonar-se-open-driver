"""Migration non destructive d'un profil v0 (configs/profiles/profile.schema.json) vers v1.

Règles (C3) : valeurs explicites conservées telles quelles, nouvel UUID,
reconstruction des groupes routing/output. Les champs sans équivalent v1
(sampleRate, masques, groupe test) ne sont jamais réactivés : ils sont
signalés. Aucune valeur n'est bornée ou corrigée en silence : une valeur hors
bornes v1 fait échouer la migration. L'original n'est jamais modifié.
"""

from __future__ import annotations

import copy
from dataclasses import dataclass, field
from typing import Any

from .model import REPO_ROOT, Manifest
from .schema_check import Issue, Validator
from .strict_json import parse_strict

MODE_MAP = {
    # mode v0 : (listeningMode, sourcePolicy, masque d'entrée attendu)
    "PassThrough": ("5.1", "preserve", {0x60F, 0x3F}),
    "Upmix2to6": ("upmix", "autoDeclaredLayout", {0x3}),
    "Downmix6to2": ("2.0", "preserve", {0x60F, 0x3F}),
    "TwoPointOne": ("2.1", "preserve", {0x3, 0x60F, 0x3F}),
}
OUTPUT_GROUPS = ("masterGainDb", "channelGainDb", "channelDelayMs", "channelMute", "eq",
                 "downmix", "twoPointOne", "bassManagement", "limiter", "gainRampMs", "maxDelayMs")


@dataclass
class MigrationResult:
    document: dict[str, Any] | None
    errors: list[Issue] = field(default_factory=list)
    warnings: list[Issue] = field(default_factory=list)
    infos: list[Issue] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.document is not None and not self.errors


def _v0_validator(m: Manifest) -> Validator:
    path = REPO_ROOT / m.raw["v0"]["schemaFile"]
    return Validator(parse_strict(path.read_bytes(), max_bytes=None, max_depth=None))


def migrate_v0(m: Manifest, doc: Any, profile_id: str) -> MigrationResult:
    from .validation import validate_value  # import local : évite un cycle

    res = MigrationResult(None)
    if isinstance(doc, dict) and "schemaVersion" in doc:
        res.errors.append(Issue("migration.notV0", "/schemaVersion", "document déjà versionné : pas un profil v0"))
        return res
    v0_issues = _v0_validator(m).validate(doc)
    if v0_issues:
        res.errors += [Issue("migration.invalidV0", i.path, f"{i.code} : {i.message}") for i in v0_issues]
        return res
    mode = doc["mode"]
    if mode == "ChannelTest":
        res.errors.append(Issue("migration.channelTestNotPersistent", "/mode",
                                "un ChannelTest n'est jamais un profil persistant en v1"))
        return res
    known = m.const("KnownChannelBits")
    for key in ("inputMask", "outputMask"):
        mask = doc[key]
        if mask & ~known:
            res.errors.append(Issue("migration.maskUnknownBits", f"/{key}",
                                    f"masque 0x{mask:X} : bits inconnus 0x{mask & ~known:X}"))
    if res.errors:
        return res
    listening, policy, inputs = MODE_MAP[mode]
    if doc["inputMask"] not in inputs:
        res.warnings.append(Issue("migration.inputMaskIgnored", "/inputMask",
                                  f"masque d'entrée 0x{doc['inputMask']:X} non conservé : le layout vient de la connexion négociée"))
    expected_out = 0x3 if mode == "Downmix6to2" else m.const("PhysicalOutputMask")
    if doc["outputMask"] != expected_out:
        res.warnings.append(Issue("migration.outputMaskIgnored", "/outputMask",
                                  f"masque de sortie 0x{doc['outputMask']:X} non conservé : sortie physique toujours 0x60F"))
    rate = doc["sampleRate"]
    if rate in m.const("XonarRenderSampleRates"):
        res.infos.append(Issue("migration.sampleRateDropped", "/sampleRate",
                               f"{rate} Hz non conservé : la fréquence vient de la connexion négociée"))
    else:
        res.warnings.append(Issue("migration.sampleRateUnsupported", "/sampleRate",
                                  f"{rate} Hz non annoncé par le rendu Xonar actuel ; non conservé, jamais activé"))
    if "test" in doc:
        res.warnings.append(Issue("migration.testGroupDropped", "/test", "groupe test abandonné : pas de ChannelTest persistant"))
    if "$schema" in doc:
        res.infos.append(Issue("migration.schemaPathDropped", "/$schema", "chemin $schema abandonné : aucun chemin dans un profil v1"))

    out: dict[str, Any] = {
        "schemaVersion": m.const("ProfileSchemaVersion"),
        "profileId": profile_id,
        "revision": 1,
    }
    for key in ("name", "description"):
        if key in doc:
            out[key] = doc[key]
    out["endpointRole"] = "speakers"
    out["listeningMode"] = listening
    out["sourcePolicy"] = policy
    out["routing"] = {"upmix": copy.deepcopy(doc["upmix"])} if "upmix" in doc else {}
    out["output"] = {k: copy.deepcopy(doc[k]) for k in OUTPUT_GROUPS if k in doc}
    issues = validate_value(out, "profile-v1#ProfileDocument")
    if issues:
        res.errors += [Issue("migration.outOfV1Bounds", i.path, f"{i.code} : {i.message}") for i in issues]
        return res
    res.document = out
    return res
