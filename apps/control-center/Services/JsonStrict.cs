using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Analyse JSON stricte pour les documents de contrat : refus des clés
/// dupliquées, des documents de plus de 128 Kio, de la profondeur > 16,
/// du contenu après la valeur racine et des nombres non finis. Miroir C# de la
/// discipline implémentée côté C++ dans src/audio-control/profiles.
/// </summary>
public static class JsonStrict
{
    public sealed class ParseResult
    {
        public bool Ok { get; init; }
        public string? DiagnosticCode { get; init; }
        public string? Message { get; init; }
        public JsonNode? Root { get; init; }

        public static ParseResult Success(JsonNode root) => new() { Ok = true, Root = root };
        public static ParseResult Fail(string code, string message) => new() { Ok = false, DiagnosticCode = code, Message = message };
    }

    /// <summary>
    /// Analyse une chaîne UTF-8 selon les règles strictes des contrats.
    /// </summary>
    public static ParseResult Parse(string json)
    {
        if (json is null)
        {
            return ParseResult.Fail("json.encoding", "Le document est vide.");
        }

        int byteCount;
        try
        {
            byteCount = Encoding.UTF8.GetByteCount(json);
        }
        catch (EncoderFallbackException)
        {
            return ParseResult.Fail("json.encoding", "Le document contient du texte UTF-16 non encodable en UTF-8.");
        }

        if (byteCount > Constants.MaxDocumentBytes)
        {
            return ParseResult.Fail("json.tooLarge", "Le document dépasse la taille maximale de 128 Kio.");
        }

        string? duplicate = FindFirstDuplicateKey(json);
        if (duplicate is not null)
        {
            return ParseResult.Fail("json.duplicateKey", $"Clé dupliquée dans un objet : « {duplicate} ».");
        }

        var options = new JsonDocumentOptions
        {
            MaxDepth = (int)Constants.MaxDocumentDepth,
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
        };

        try
        {
            JsonNode? root = JsonNode.Parse(json, nodeOptions: null, documentOptions: options);
            if (root is null)
            {
                return ParseResult.Fail("json.syntax", "Le document est vide.");
            }
            return ParseResult.Success(root);
        }
        catch (JsonException ex)
        {
            if (ContainsDepthError(ex))
            {
                return ParseResult.Fail("json.tooDeep", "L'imbrication dépasse la profondeur maximale de 16.");
            }
            return ParseResult.Fail("json.syntax", "JSON invalide : " + Truncate(ex.Message));
        }
        catch (ArgumentException)
        {
            return ParseResult.Fail("json.syntax", "JSON invalide ou profondeur maximale dépassée.");
        }
    }

    private static bool ContainsDepthError(JsonException ex)
    {
        // JsonException n'expose pas de code stable ; la profondeur est détectée
        // par le message « MaxDepth » ou via une chaîne très profonde.
        return ex.Message.Contains("MaxDepth", StringComparison.OrdinalIgnoreCase)
            || ex.Message.Contains("depth", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Détecte la première clé dupliquée dans un objet JSON, en ignorant le
    /// contenu des chaînes et les échappements. Retourne null si aucune.
    /// </summary>
    public static string? FindFirstDuplicateKey(string json)
    {
        var keySets = new Stack<HashSet<string>>();
        int i = 0;
        int n = json.Length;

        while (i < n)
        {
            char c = json[i];
            switch (c)
            {
                case '{':
                    keySets.Push(new HashSet<string>(StringComparer.Ordinal));
                    i++;
                    break;

                case '}':
                    if (keySets.Count > 0)
                    {
                        keySets.Pop();
                    }
                    i++;
                    break;

                case '"':
                    {
                        int start = i;
                        i = SkipString(json, i);
                        if (i < 0)
                        {
                            return null; // chaîne invalide : l'analyse JSON échouera ensuite
                        }
                        string token = json.Substring(start + 1, i - start - 1);
                        // Avance jusqu'au prochain caractère non blanc.
                        int j = i;
                        while (j < n && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n'))
                        {
                            j++;
                        }
                        if (j < n && json[j] == ':' && keySets.Count > 0)
                        {
                            if (!keySets.Peek().Add(token))
                            {
                                return token;
                            }
                        }
                        i = j;
                        break;
                    }

                default:
                    i++;
                    break;
            }
        }

        return null;
    }

    private static int SkipString(string json, int start)
    {
        // start pointe sur le guillemet ouvrant.
        int i = start + 1;
        int n = json.Length;
        while (i < n)
        {
            char c = json[i];
            if (c == '\\')
            {
                i += 2;
                if (i > n)
                {
                    return -1;
                }
                continue;
            }
            if (c == '"')
            {
                return i + 1;
            }
            i++;
        }
        return -1;
    }

    private static string Truncate(string message)
    {
        const int max = 120;
        return message.Length <= max ? message : message.Substring(0, max) + "…";
    }
}
