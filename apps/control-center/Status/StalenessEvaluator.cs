using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Status;

/// <summary>
/// Évaluation pure de la péremption des mesures (C7) : une mesure est « périmée »
/// si son âge dépasse 2 s ou si l'agent la marque explicitement périmée. Aucune
/// mesure périmée n'est présentée comme du silence.
/// </summary>
public static class StalenessEvaluator
{
    /// <summary>Détermine si une mesure est périmée.</summary>
    public static bool IsStale(MeterState? meters, TimeSpan sinceLastNotification)
    {
        if (meters is null)
        {
            return true;
        }
        if (meters.Stale)
        {
            return true;
        }
        if (meters.AgeMs > Constants.TelemetryStaleMs)
        {
            return true;
        }
        if (sinceLastNotification.TotalMilliseconds > Constants.TelemetryStaleMs)
        {
            return true;
        }
        return false;
    }

    /// <summary>Détermine si le format négocié a dérivé du 5.1 / 0x60F attendu.</summary>
    public static bool HasFormatDrift(NegotiatedFormat? format)
    {
        return format is not null
            && (format.ChannelMask != Constants.Mask51Side
                || format.ChannelCount != Constants.PhysicalOutputChannels);
    }
}
