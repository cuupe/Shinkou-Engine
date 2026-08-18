# ShinkouEngine C# scripting

This directory is the managed scripting boundary for ShinkouEngine. It is independent of
the renderer and mirrors the native `World`, `GameObject`, `Component`, and `Registry`
contracts.

- `Shinkou.Scripting` provides OOP scene objects, GameObject components, generational ECS
  entities/components, ordered ECS systems, and deferred destruction.
- `Shinkou.ScriptCompiler` compiles one or more C# script files into a managed assembly
  with Roslyn and automatically references the installed .NET platform plus
  `Shinkou.Scripting`.
- `Shinkou.Scripting.Tests` is a dependency-light lifecycle regression executable.

Build and test from this directory:

```powershell
dotnet build Shinkou.Scripting.sln
dotnet run --project Shinkou.Scripting.Tests
dotnet run --project Shinkou.ScriptCompiler -- --source path\to\MyScript.cs --output build\scripts.dll
```

User scripts should derive from `ScriptComponent` for OOP/GameObject behaviour or
`EcsSystem` for ECS update logic. Destruction is requested immediately and collected at
the end of the current world update, matching the native engine's lifetime rule.
