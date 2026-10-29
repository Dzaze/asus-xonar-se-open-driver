namespace Xonar.ControlCenter.Tests;

public static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--render-probe"))
        {
            using var endpoint = Xonar.ControlCenter.Services.RenderAudioEndpoint.Resolve("Xonar");
            if (endpoint is null) { Console.WriteLine("Endpoint introuvable"); return 1; }
            Console.WriteLine("Endpoint : " + endpoint.FriendlyName);
            bool ok = endpoint.PlayChannelTestSequenceAsync(new[] { 0, 1, 2, 3, 4, 5 },
                channel => Console.WriteLine("Canal " + channel), .25, 440, -35, CancellationToken.None).GetAwaiter().GetResult();
            Console.WriteLine(ok ? "TEST ENCEINTES OK" : endpoint.LastTestError);
            return ok ? 0 : 1;
        }
        Console.WriteLine("Tests du centre de contrôle (faux agent, sans carte ni pilote).");
        ShellTests.RunAll();
        ProfilesTests.RunAll();
        StatusTests.RunAll();
        ApoEfxConfigTests.RunAll();
        ProfileLibraryTests.RunAll();
        LiveRuntimeTests.RunAll();
        return Test.Summary();
    }
}
