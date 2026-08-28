using System.IO;
using System.Text;

namespace ShinkouUI.Studio.Wpf;

public partial class App : System.Windows.Application
{
    private int _startupFailureReported;

    public App()
    {
        DispatcherUnhandledException += (_, args) =>
        {
            ReportStartupFailure(args.Exception);
            args.Handled = true;
            Shutdown(1);
        };

        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            if (args.ExceptionObject is Exception exception)
                ReportStartupFailure(exception);
        };
    }

    private void ReportStartupFailure(Exception exception)
    {
        if (Interlocked.Exchange(ref _startupFailureReported, 1) != 0)
            return;

        var details = new StringBuilder();
        details.AppendLine("ShinkouUI Studio 启动失败");
        details.AppendLine();
        details.AppendLine(Unwrap(exception).ToString());

        try
        {
            var logPath = Path.Combine(AppContext.BaseDirectory, "startup-error.log");
            File.WriteAllText(logPath, details.ToString(), Encoding.UTF8);
            details.AppendLine();
            details.AppendLine($"详细日志：{logPath}");
        }
        catch
        {
            // The error dialog must still be shown when the output folder is read-only.
        }

        System.Windows.MessageBox.Show(
            details.ToString(),
            "ShinkouUI Studio",
            System.Windows.MessageBoxButton.OK,
            System.Windows.MessageBoxImage.Error);
    }

    private static Exception Unwrap(Exception exception)
    {
        while (exception.InnerException is not null &&
               exception is System.Reflection.TargetInvocationException or TypeInitializationException)
        {
            exception = exception.InnerException;
        }

        return exception;
    }
}
