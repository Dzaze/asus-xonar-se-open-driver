namespace Xonar.ControlCenter.Services;

/// <summary>
/// Coalesceur de commandes borné : garantit que les commandes émises pour une
/// même clé (ex. un curseur de trim) ne dépassent pas <paramref name="maxCommandsPerSecond"/>
/// commandes par seconde. Seule la valeur la plus récente de chaque clé est
/// conservée ; aucune file non bornée, aucune attente sur le thread audio.
/// </summary>
public sealed class CommandCoalescer : IDisposable
{
    private readonly object _gate = new();
    private readonly Dictionary<string, Func<Task>> _pending = new();
    private readonly System.Threading.Timer _timer;
    private readonly int _periodMs;

    public CommandCoalescer(int maxCommandsPerSecond = 10)
    {
        if (maxCommandsPerSecond <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maxCommandsPerSecond));
        }
        _periodMs = Math.Max(1, 1000 / maxCommandsPerSecond);
        _timer = new System.Threading.Timer(Flush, null, Timeout.Infinite, Timeout.Infinite);
    }

    /// <summary>Nombre de commandes exécutées depuis la création (pour les tests).</summary>
    public int ExecutedCount { get; private set; }

    /// <summary>Période effective entre deux rafales de commandes (pour les tests).</summary>
    public int PeriodMs => _periodMs;

    /// <summary>
    /// Enregistre une commande à exécuter ; remplace toute commande en attente
    /// portant la même clé.
    /// </summary>
    public void Enqueue(string key, Func<Task> command)
    {
        lock (_gate)
        {
            _pending[key] = command;
            _timer.Change(_periodMs, Timeout.Infinite);
        }
    }

    private void Flush(object? _)
    {
        List<Func<Task>> toRun;
        lock (_gate)
        {
            toRun = _pending.Values.ToList();
            _pending.Clear();
        }

        // Chaque commande est lancée via un garde-fou asynchrone : une commande
        // fautive (y compris une exception à affinité de thread) est absorbée et ne
        // termine JAMAIS le processus. On n'utilise plus d'Action/async void (conf#0).
        foreach (var command in toRun)
        {
            _ = RunGuardedAsync(command);
        }
    }

    private async Task RunGuardedAsync(Func<Task> command)
    {
        try
        {
            await command();
        }
        catch
        {
            // Garde-fou du coalesceur : la commande gère ses propres erreurs métier ;
            // tout ce qui remonte jusqu'ici est absorbé pour ne pas crasher l'appli.
        }
        finally
        {
            lock (_gate)
            {
                ExecutedCount++;
            }
        }
    }

    public void Dispose()
    {
        _timer.Dispose();
    }
}
