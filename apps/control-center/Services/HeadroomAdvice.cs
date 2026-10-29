namespace Xonar.ControlCenter.Services;

/// <summary>Conseil issu de la crête avant protection ; aucune modification automatique du profil.</summary>
public sealed record HeadroomAdvice(string Message, double AdjustmentDb)
{
    public static HeadroomAdvice From(DspLiveStatus? status)
    {
        if(status is null || !status.Enabled || !float.IsFinite(status.PreLimiterPeak) || status.PreLimiterPeak < 0)
            return new("Mesure indisponible", 0);
        double peakDb = status.PreLimiterPeak > 0 ? 20 * Math.Log10(status.PreLimiterPeak) : -200;
        double reduction = float.IsFinite(status.LimiterReductionDb) ? Math.Max(0, status.LimiterReductionDb) : 0;
        bool overloaded = peakDb > 0 || reduction > .01;
        double adjustment = overloaded ? -Math.Min(60, Math.Max(Math.Max(0, peakDb), reduction) + 1) : 0;
        string peak = peakDb <= -200 ? "silence" : $"{peakDb:0.0} dBFS";
        return new($"Crête avant protection : {peak}. Écrêtages depuis l’ouverture : {status.ClippedSamples}." +
            (reduction > .01 ? $" Réduction limiteur : {reduction:0.0} dB." : "") +
            (overloaded ? $" Atténuation conseillée : {adjustment:0.0} dB." : ""), adjustment);
    }
    public double ApplyTo(double masterDb) => Math.Clamp(masterDb + AdjustmentDb, -60, 0);
}
