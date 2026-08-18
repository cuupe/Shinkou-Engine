using System.Reflection;
using System.Runtime.Loader;

namespace Shinkou.Engine.Scripting;

public sealed class ScriptAssembly : IDisposable
{
    private readonly AssemblyLoadContext loadContext;
    private readonly Assembly assembly;
    private bool disposed;

    private ScriptAssembly(string path)
    {
        var assemblyPath = Path.GetFullPath(path);
        loadContext = new AssemblyLoadContext($"ShinkouScripts:{Path.GetFileNameWithoutExtension(assemblyPath)}", isCollectible: true);
        assembly = loadContext.LoadFromAssemblyPath(assemblyPath);
    }

    public IReadOnlyList<Type> ComponentTypes => GetTypes(typeof(ScriptComponent));
    public IReadOnlyList<Type> SystemTypes => GetTypes(typeof(EcsSystem));

    public static ScriptAssembly Load(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("Compiled script assembly was not found.", path);
        return new ScriptAssembly(path);
    }

    public ScriptComponent CreateComponent(string typeName)
    {
        var type = FindConcreteType(ComponentTypes, typeName);
        return (ScriptComponent)Activator.CreateInstance(type)!;
    }

    public EcsSystem CreateSystem(string typeName)
    {
        var type = FindConcreteType(SystemTypes, typeName);
        return (EcsSystem)Activator.CreateInstance(type)!;
    }

    public void Dispose()
    {
        if (disposed)
            return;
        disposed = true;
        loadContext.Unload();
    }

    private IReadOnlyList<Type> GetTypes(Type baseType) => assembly.GetTypes()
        .Where(type => baseType.IsAssignableFrom(type) && type != baseType && !type.IsAbstract && type.GetConstructor(Type.EmptyTypes) is not null)
        .OrderBy(type => type.FullName, StringComparer.Ordinal)
        .ToArray();

    private static Type FindConcreteType(IEnumerable<Type> types, string typeName) =>
        types.FirstOrDefault(type => string.Equals(type.FullName, typeName, StringComparison.Ordinal) ||
                                     string.Equals(type.Name, typeName, StringComparison.Ordinal))
        ?? throw new InvalidOperationException($"Script type '{typeName}' was not found or has no public parameterless constructor.");
}
