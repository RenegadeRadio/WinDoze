using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using GameCrate.Gui.Services;
using Microsoft.Win32;
using MessageBox = System.Windows.MessageBox;
using OpenFileDialog = Microsoft.Win32.OpenFileDialog;

namespace GameCrate.Gui;

public partial class InstallWindow : Window
{
    private readonly GameCrateService _service;
    private bool _installing;

    public InstallWindow(GameCrateService service)
    {
        _service = service;
        InitializeComponent();
    }

    private void BrowseInstallDir_Click(object sender, RoutedEventArgs e)
    {
        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = "Choose an empty folder for the game install"
        };

        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            InstallDirBox.Text = dialog.SelectedPath;
        }
    }

    private void BrowseInstaller_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Installers (*.exe)|*.exe|All files (*.*)|*.*",
            Title = "Select installer"
        };

        if (dialog.ShowDialog() == true)
        {
            InstallerBox.Text = dialog.FileName;
            if (string.IsNullOrWhiteSpace(NameBox.Text))
            {
                NameBox.Text = Path.GetFileNameWithoutExtension(dialog.FileName);
            }

            if (string.IsNullOrWhiteSpace(IdBox.Text) && !string.IsNullOrWhiteSpace(NameBox.Text))
            {
                IdBox.Text = Slugify(NameBox.Text);
            }
        }
    }

    private static string Slugify(string value)
    {
        var slug = Regex.Replace(value.ToLowerInvariant(), @"[^a-z0-9]+", "-").Trim('-');
        return slug.Length > 0 ? slug : "game";
    }

    private async void InstallStartButton_Click(object sender, RoutedEventArgs e)
    {
        if (_installing)
        {
            return;
        }

        var name = NameBox.Text.Trim();
        var id = IdBox.Text.Trim();
        var installDir = InstallDirBox.Text.Trim();
        var installer = InstallerBox.Text.Trim();

        if (string.IsNullOrEmpty(name) || string.IsNullOrEmpty(id) ||
            string.IsNullOrEmpty(installDir) || string.IsNullOrEmpty(installer))
        {
            MessageBox.Show("Fill in all fields.", "Install", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!File.Exists(installer))
        {
            MessageBox.Show("Installer file not found.", "Install", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!Regex.IsMatch(id, @"^[a-z0-9][a-z0-9\-]*$"))
        {
            MessageBox.Show("Profile ID must start with a lowercase letter or number and contain only lowercase letters, numbers, and hyphens.",
                "Install", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        string? installPathError = InstallPathValidator.Validate(installDir);
        if (installPathError != null)
        {
            MessageBox.Show(installPathError, "Install folder", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        installDir = Path.GetFullPath(installDir)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        _installing = true;
        InstallStartButton.IsEnabled = false;
        CancelButton.IsEnabled = false;
        InstallProgress.Visibility = Visibility.Visible;
        InstallProgress.IsIndeterminate = true;
        InstallStatus.Text = "Running installer (monitored) — approve UAC if prompted...";

        try
        {
            var result = await _service.InstallAsync(
                id,
                name,
                installDir,
                installer,
                virtualizeAppData: VirtualAppDataCheck.IsChecked == true,
                allowNetwork: NetworkCheck.IsChecked == true,
                strictOutsideWrites: StrictOutsideWritesCheck.IsChecked == true);

            InstallProgress.IsIndeterminate = false;
            InstallProgress.Visibility = Visibility.Collapsed;

            if (result.Success)
            {
                InstallStatus.Text = "Install finished. Review the install report from the main window.";
                MessageBox.Show(
                    "Install completed.\n\nSelect the profile and click Install report to review outside writes.",
                    "Install complete",
                    MessageBoxButton.OK,
                    MessageBoxImage.Information);
                DialogResult = true;
                Close();
                return;
            }

            if (await TryRecoverMissingExecutableAsync(id, installDir, result))
            {
                return;
            }

            InstallStatus.Text = "Install failed.";
            MessageBox.Show(
                result.FormatOutput("Install failed. Check the install report or run gamecrate install from PowerShell."),
                "Install failed",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
        }
        catch (Exception ex)
        {
            InstallStatus.Text = "Install failed.";
            MessageBox.Show(
                ex.Message,
                "Install failed",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
        }
        finally
        {
            _installing = false;
            InstallStartButton.IsEnabled = true;
            CancelButton.IsEnabled = true;
            InstallProgress.Visibility = Visibility.Collapsed;
        }
    }

    private async Task<bool> TryRecoverMissingExecutableAsync(
        string profileId,
        string installDir,
        GameCrateResult installResult)
    {
        string combined = (installResult.StandardOutput + installResult.StandardError).ToLowerInvariant();
        if (!combined.Contains("no game executable was detected"))
        {
            return false;
        }

        var pick = MessageBox.Show(
            "The installer finished, but GameCrate could not find the game .exe automatically.\n\n" +
            "Pick the game's main executable now?",
            "Select game executable",
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);

        if (pick != MessageBoxResult.Yes)
        {
            return false;
        }

        string initialDir = Directory.Exists(installDir) ? installDir : string.Empty;
        var dialog = new OpenFileDialog
        {
            Filter = "Game executables (*.exe)|*.exe|All files (*.*)|*.*",
            Title = "Select game executable",
            InitialDirectory = string.IsNullOrEmpty(initialDir) ? null : initialDir,
        };

        if (dialog.ShowDialog() != true)
        {
            return false;
        }

        InstallStatus.Text = "Saving executable path...";
        var setResult = await _service.SetExecutableAsync(profileId, dialog.FileName);
        if (!setResult.Success)
        {
            MessageBox.Show(
                setResult.FormatOutput("Failed to set executable."),
                "Install incomplete",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            return false;
        }

        InstallStatus.Text = "Install finished with manual executable.";
        MessageBox.Show(
            $"Executable set to:\n{dialog.FileName}\n\nYou can Play from the main window.",
            "Install complete",
            MessageBoxButton.OK,
            MessageBoxImage.Information);
        DialogResult = true;
        Close();
        return true;
    }

    private void CancelButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_installing)
        {
            DialogResult = false;
            Close();
        }
    }
}
