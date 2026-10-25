// SPDX-License-Identifier: MPL-2.0

using System.Windows.Forms;

namespace MapWinGIS.ActiveXTestHost;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            HostOptions options = HostOptions.Parse(args);
            ApplicationConfiguration.Initialize();

            using var form = new MainForm(options);
            Application.Run(form);
            return form.ExitCode;
        }
        catch (Exception exception)
        {
            MessageBox.Show(
                exception.Message,
                "MapWinGIS ActiveX Test Host",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
            return 1;
        }
    }
}

internal sealed record HostOptions(bool Automated, string ResultsPath, int TimeoutSeconds)
{
    public static HostOptions Parse(string[] args)
    {
        bool automated = false;
        string? resultsPath = null;
        int timeoutSeconds = 30;

        for (int index = 0; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--automated":
                    automated = true;
                    break;
                case "--results" when index + 1 < args.Length:
                    resultsPath = args[++index];
                    break;
                case "--timeout-seconds" when index + 1 < args.Length:
                    if (!int.TryParse(args[++index], out timeoutSeconds) ||
                        timeoutSeconds is < 5 or > 300)
                    {
                        throw new ArgumentException(
                            "--timeout-seconds must be between 5 and 300.");
                    }
                    break;
                default:
                    throw new ArgumentException($"Unknown or incomplete option: {args[index]}");
            }
        }

        if (!automated && (resultsPath is not null || timeoutSeconds != 30))
        {
            throw new ArgumentException(
                "--results and --timeout-seconds require --automated.");
        }

        resultsPath ??= Path.Combine(
            AppContext.BaseDirectory,
            "ActiveXTestHost-results.json");
        return new HostOptions(
            automated,
            Path.GetFullPath(resultsPath),
            timeoutSeconds);
    }
}
