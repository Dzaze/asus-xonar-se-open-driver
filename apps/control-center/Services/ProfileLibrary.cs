using System.Text;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>Profils personnels en JSON v1 validé. Les noms de fichiers sont des UUID.</summary>
public sealed class ProfileLibrary
{
    public string DirectoryPath { get; }
    public ProfileLibrary(string? directory = null) => DirectoryPath = Path.GetFullPath(directory ??
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "XonarSE", "Profiles"));

    public IReadOnlyList<ProfileDocument> List()
    {
        if (!Directory.Exists(DirectoryPath)) return Array.Empty<ProfileDocument>();
        var profiles = new List<ProfileDocument>();
        foreach(string path in Directory.EnumerateFiles(DirectoryPath, "*.json")) {
            try { profiles.Add(Read(path)); } catch (Exception ex) when(ex is IOException or FormatException or UnauthorizedAccessException) { }
        }
        return profiles.OrderBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase).ToArray();
    }

    public static ProfileDocument Read(string path)
    {
        if (new FileInfo(path).Length > Constants.MaxDocumentBytes) throw new FormatException("Le profil dépasse 128 Kio.");
        var parsed = JsonStrict.Parse(File.ReadAllText(path));
        if (!parsed.Ok) throw new FormatException(parsed.Message);
        var result = ProfileValidator.Validate(parsed.Root!);
        if (!result.Valid || result.Normalized is null)
            throw new FormatException(string.Join(" ; ", result.Diagnostics.Select(d => d.Message)));
        return result.Normalized;
    }

    public static void Export(ProfileDocument profile, string path)
    {
        var node = ContractJson.Profile(profile);
        var result = ProfileValidator.Validate(node);
        if (!result.Valid) throw new FormatException("Ce profil n'est pas valide pour l'export.");
        File.WriteAllText(path, node.ToJsonString(new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerOptions.Default) { WriteIndented = true }), new UTF8Encoding(false));
    }

    public void Save(ProfileDocument profile)
    {
        Directory.CreateDirectory(DirectoryPath);
        string path = OwnPath(profile.ProfileId);
        Export(profile, path + ".tmp");
        File.Move(path + ".tmp", path, true);
    }

    public void Delete(string profileId) => File.Delete(OwnPath(profileId));
    private string OwnPath(string id) => Path.Combine(DirectoryPath, Guid.Parse(id).ToString("N") + ".json");
}
