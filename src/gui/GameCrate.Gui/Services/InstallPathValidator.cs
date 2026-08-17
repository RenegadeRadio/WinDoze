using System.IO;

namespace GameCrate.Gui.Services;

internal static class InstallPathValidator
{
    public static string? Validate(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return "Install directory is required.";
        }

        if (path.Contains('"'))
        {
            return "Install directory cannot contain a quotation mark.";
        }

        string normalized = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string lower = normalized.ToLowerInvariant();

        if (lower.Contains("\\program files") || lower.Contains("\\windows\\") || lower.EndsWith("\\windows"))
        {
            return "Install directory cannot be under Program Files or Windows.";
        }

        string root = Path.GetPathRoot(normalized) ?? string.Empty;
        if (string.IsNullOrEmpty(root) || normalized.Length <= root.Length)
        {
            return "Do not install directly to a drive root. Use a subfolder like D:\\Games\\MyGame.";
        }

        string relative = normalized[root.Length..].TrimStart('\\');
        if (!relative.Contains(Path.DirectorySeparatorChar) && !relative.Contains(Path.AltDirectorySeparatorChar))
        {
            return $"Install folder \"{normalized}\" is directly on the drive root (e.g. C:\\Install). " +
                   "Windows often blocks sandbox ACL changes there.\n\n" +
                   "Use a folder you own instead, e.g.:\n" +
                   $"  {Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Games", "my-game")}\n" +
                   "  D:\\Games\\my-game";
        }

        return null;
    }
}
