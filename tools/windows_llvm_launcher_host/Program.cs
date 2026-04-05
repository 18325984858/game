using System.Diagnostics;

static string? ResolveResourceDir(string llvmRoot)
{
    foreach (var candidate in new[]
    {
        Path.Combine(llvmRoot, "lib", "clang"),
        Path.Combine(llvmRoot, "clang")
    })
    {
        if (!Directory.Exists(candidate))
        {
            continue;
        }

        var resourceDir = Directory.GetDirectories(candidate)
            .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault();
        if (resourceDir is not null)
        {
            return resourceDir.Replace('\\', '/');
        }
    }

    return null;
}

if (args.Length == 0)
{
    Console.Error.WriteLine("[windows_llvm_launcher] expected the original compiler as the first argument.");
    return 1;
}

var launcherDir = AppContext.BaseDirectory;
var llvmRoot = Environment.GetEnvironmentVariable("WINDOWS_LLVM_ROOT");
if (string.IsNullOrWhiteSpace(llvmRoot))
{
    llvmRoot = Path.GetFullPath(Path.Combine(launcherDir, "..", "..", "Windows-llvm"));
}

var originalCompiler = Path.GetFileName(args[0]);
var frontendName = string.Equals(originalCompiler, "clang++.exe", StringComparison.OrdinalIgnoreCase)
    ? "clang++.exe"
    : "clang.exe";
var frontendPath = Path.Combine(llvmRoot, "bin", frontendName);
if (!File.Exists(frontendPath))
{
    Console.Error.WriteLine($"[windows_llvm_launcher] frontend not found: {frontendPath}");
    return 1;
}

var startInfo = new ProcessStartInfo(frontendPath)
{
    UseShellExecute = false,
};

var existingPath = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
startInfo.Environment["PATH"] = string.Join(";", new[]
{
    Path.Combine(llvmRoot, "bin"),
    llvmRoot,
    existingPath,
}.Where(value => !string.IsNullOrWhiteSpace(value)));

var resourceDir = ResolveResourceDir(llvmRoot);
if (!string.IsNullOrWhiteSpace(resourceDir))
{
    startInfo.ArgumentList.Add($"-resource-dir={resourceDir}");
}

foreach (var arg in args.Skip(1))
{
    startInfo.ArgumentList.Add(arg);
}

using var process = Process.Start(startInfo);
if (process is null)
{
    Console.Error.WriteLine("[windows_llvm_launcher] failed to start the frontend process.");
    return 1;
}

process.WaitForExit();
if (process.ExitCode == unchecked((int)0xC0000135))
{
    Console.Error.WriteLine("[windows_llvm_launcher] frontend failed to start with 0xC0000135. A required runtime DLL is missing.");
}

return process.ExitCode;