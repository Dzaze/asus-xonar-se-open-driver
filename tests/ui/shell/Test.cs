namespace Xonar.ControlCenter.Tests;

/// <summary>Mini-cadre de test sans dépendance externe.</summary>
public static class Test
{
    public static int Failed { get; private set; }
    public static int Passed { get; private set; }

    public static void Run(string name, Action body)
    {
        try
        {
            body();
            Passed++;
            Console.WriteLine($"[OK]   {name}");
        }
        catch (Exception ex)
        {
            Failed++;
            Console.WriteLine($"[ECHEC] {name} : {ex.Message}");
        }
    }

    public static void Assert(bool condition, string message = "Assertion échouée.")
    {
        if (!condition)
        {
            throw new Exception(message);
        }
    }

    public static void Equal<T>(T expected, T actual, string? message = null)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new Exception(message ?? $"Attendu {expected}, obtenu {actual}.");
        }
    }

    public static T Throws<T>(Action action, string? message = null) where T : Exception
    {
        try
        {
            action();
        }
        catch (T ex)
        {
            return ex;
        }
        throw new Exception(message ?? $"Exception {typeof(T).Name} attendue, aucune levée.");
    }

    public static int Summary()
    {
        Console.WriteLine($"\n{Passed} test(s) OK, {Failed} en échec.");
        // Code de sortie = nombre d'échecs (ce qu'attend l'opérateur : $LASTEXITCODE).
        // Plafonné à 255 car un code de sortie de processus tient sur un octet.
        return Math.Min(Failed, 255);
    }
}
