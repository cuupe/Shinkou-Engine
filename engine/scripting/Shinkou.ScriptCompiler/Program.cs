using Shinkou.ScriptCompiler;

if (args.Length == 0 || args.Contains("--help", StringComparer.OrdinalIgnoreCase))
{
    Console.WriteLine("Usage: Shinkou.ScriptCompiler --source <file.cs> [--source <file.cs> ...] --output <scripts.dll> [--reference <assembly.dll>]");
    return args.Length == 0 ? 2 : 0;
}

var sources = Values("--source").ToArray();
var references = Values("--reference").ToArray();
var output = SingleValue("--output");
if (sources.Length == 0 || string.IsNullOrWhiteSpace(output))
{
    Console.Error.WriteLine("Both --source and --output are required.");
    return 2;
}

var result = new CSharpScriptCompiler().Compile(sources, output, references);
foreach (var diagnostic in result.Diagnostics)
    Console.Error.WriteLine(diagnostic);
return result.Success ? 0 : 1;

IEnumerable<string> Values(string name)
{
    for (var index = 0; index < args.Length - 1; ++index)
        if (string.Equals(args[index], name, StringComparison.OrdinalIgnoreCase))
            yield return args[++index];
}

string? SingleValue(string name) => Values(name).FirstOrDefault();
