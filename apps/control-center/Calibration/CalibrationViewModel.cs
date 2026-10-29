using Xonar.Contracts.V1;
using Xonar.ControlCenter.Profiles;
using Xonar.ControlCenter.Services;

namespace Xonar.ControlCenter.Calibration;

/// <summary>
/// Modèle de la page Calibration. Délègue l'édition au
/// <see cref="ProfileEditorViewModel"/> partagé ; expose uniquement des bornes
/// et des libellés honnêtes, sans écrire dans KS ni dans CAPX.
/// </summary>
public sealed class CalibrationViewModel
{
    public CalibrationViewModel(ProfileEditorViewModel editor)
    {
        Editor = editor;
    }

    public ProfileEditorViewModel Editor { get; }

    /// <summary>Volume matériel : jamais modifié par les profils ni par cette page.</summary>
    public string HardwareVolumeNote =>
        "Le volume matériel Windows (KS/UAC2) est un domaine distinct des gains DSP. " +
        "Cette page ne modifie que les gains DSP du profil ; l'annulation d'un profil " +
        "ne réinitialise jamais le volume système.";
}
