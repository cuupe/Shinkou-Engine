using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;
using System.Text;
using Shinkou.Engine.Scripting;

namespace Shinkou.ScriptCompiler;

public sealed record ScriptCompilationResult(bool Success, string AssemblyPath, IReadOnlyList<string> Diagnostics);

public sealed class CSharpScriptCompiler
{
    public ScriptCompilationResult Compile(
        IEnumerable<string> sourceFiles,
        string outputAssembly,
        IEnumerable<string>? additionalReferences = null,
        bool emitDebugSymbols = true)
    {
        var files = sourceFiles.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        if (files.Length == 0)
            throw new ArgumentException("At least one C# source file is required.", nameof(sourceFiles));

        foreach (var file in files)
            if (!File.Exists(file))
                throw new FileNotFoundException("C# source file was not found.", file);

        var trees = files.Select(file => CSharpSyntaxTree.ParseText(
            SourceText.From(File.ReadAllText(file), Encoding.UTF8),
            new CSharpParseOptions(LanguageVersion.Latest),
            file)).ToArray();
        var references = CreateReferences(additionalReferences);
        var assemblyPath = Path.GetFullPath(outputAssembly);
        Directory.CreateDirectory(Path.GetDirectoryName(assemblyPath)!);
        var compilation = CSharpCompilation.Create(
            Path.GetFileNameWithoutExtension(assemblyPath),
            trees,
            references,
            new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: OptimizationLevel.Release,
                nullableContextOptions: NullableContextOptions.Enable,
                deterministic: true));

        using var assemblyStream = File.Create(assemblyPath);
        using var pdbStream = emitDebugSymbols
            ? File.Create(Path.ChangeExtension(assemblyPath, ".pdb"))
            : null;
        var emit = compilation.Emit(assemblyStream, pdbStream);
        var diagnostics = emit.Diagnostics
            .Where(diagnostic => diagnostic.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning)
            .Select(diagnostic => diagnostic.ToString())
            .ToArray();
        return new ScriptCompilationResult(emit.Success, assemblyPath, diagnostics);
    }

    private static IEnumerable<MetadataReference> CreateReferences(IEnumerable<string>? additionalReferences)
    {
        var trustedPlatformAssemblies = (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string ?? string.Empty)
            .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
            .Distinct(StringComparer.OrdinalIgnoreCase);
        foreach (var reference in trustedPlatformAssemblies)
            yield return MetadataReference.CreateFromFile(reference);

        var runtimeAssembly = typeof(ScriptComponent).Assembly.Location;
        if (!string.IsNullOrWhiteSpace(runtimeAssembly))
            yield return MetadataReference.CreateFromFile(runtimeAssembly);
        if (additionalReferences is null)
            yield break;
        foreach (var reference in additionalReferences.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase))
            yield return MetadataReference.CreateFromFile(reference);
    }
}
