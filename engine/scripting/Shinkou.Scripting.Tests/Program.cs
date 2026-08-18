using Shinkou.Engine.Scripting;

var tests = new ScriptTests();
tests.ComponentLifecycleMatchesNativeWorld();
tests.EcsGenerationAndSystemLifecycleWork();
Console.WriteLine("C# scripting tests passed");

sealed class ProbeComponent : ScriptComponent
{
    public static List<string>? Events { get; set; }
    private static List<string> Log => Events ?? throw new InvalidOperationException("Probe event sink is not configured.");
    protected override void OnAttach() => Log.Add("attach");
    protected override void OnCreate() => Log.Add("create");
    protected override void OnEnable() => Log.Add("enable");
    protected override void OnDisable() => Log.Add("disable");
    protected override void OnUpdate(float _) => Log.Add("update");
    protected override void OnDestroyRequested() => Log.Add("destroy-requested");
    protected override void OnDestroy() => Log.Add("destroy");
}

sealed class ProbeSystem : EcsSystem
{
    private readonly List<string> events;
    public ProbeSystem(List<string> values) => events = values;
    public override int Order => 10;
    protected override void OnCreate() => events.Add("system-create");
    protected override void OnEnable() => events.Add("system-enable");
    protected override void OnUpdate(float _) => events.Add("system-update");
    protected override void OnDestroy() => events.Add("system-destroy");
}

public sealed class LoadableComponent : ScriptComponent { }

sealed class ScriptTests
{
    public void ComponentLifecycleMatchesNativeWorld()
    {
        using var world = new World();
        var events = new List<string>();
        ProbeComponent.Events = events;
        var parent = world.CreateObject("parent");
        var child = parent.CreateChild("child");
        var probe = child.AddComponent<ProbeComponent>();
        Assert.Sequence(events, "attach", "create", "enable");

        world.Update(0.016f);
        Assert.Sequence(events, "attach", "create", "enable", "update");
        parent.SetActive(false);
        Assert.Equal(1, events.Count(value => value == "disable"));
        parent.SetActive(true);
        Assert.Equal(2, events.Count(value => value == "enable"));

        child.Destroy();
        Assert.Equal(1, events.Count(value => value == "destroy-requested"));
        Assert.Equal(1, events.Count(value => value == "destroy"));
        Assert.True(!child.Handle.Valid, "Destroyed child must not remain in the world lookup.");
        Assert.True(probe.LifecycleState == LifecycleState.Destroyed, "Destroyed component must reach Destroyed state.");
    }

    public void EcsGenerationAndSystemLifecycleWork()
    {
        using var world = new World();
        var events = new List<string>();
        var system = new ProbeSystem(events);
        world.AddSystem(system);
        Assert.Sequence(events, "system-create", "system-enable");

        var objectWithEntity = world.CreateObject("ecs");
        var first = objectWithEntity.EnableEcs();
        objectWithEntity.AddEcsComponent(new Position(3));
        Assert.Equal(1, world.Ecs.Query<Position>().Count());
        objectWithEntity.DisableEcs();
        var second = objectWithEntity.EnableEcs();
        Assert.True(first.Id == second.Id && first.Generation != second.Generation, "ECS IDs must use generations.");
        Assert.True(!world.Ecs.Valid(first) && world.Ecs.Valid(second), "Stale ECS handles must be rejected.");

        world.Update(0.016f);
        Assert.Equal(1, events.Count(value => value == "system-update"));
        world.ClearSystems();
        Assert.Equal(1, events.Count(value => value == "system-destroy"));

        using var assembly = ScriptAssembly.Load(typeof(LoadableComponent).Assembly.Location);
        using var mountedWorld = new World();
        mountedWorld.MountScriptAssembly(assembly);
        var loadedObject = mountedWorld.CreateObject("loaded");
        var loaded = loadedObject.AddComponent(typeof(LoadableComponent).FullName!);
        Assert.True(loaded?.GetType().FullName == typeof(LoadableComponent).FullName,
            "Compiled assemblies must expose constructible script components.");
    }

    private sealed record Position(int Value);
}

static class Assert
{
    public static void True(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    public static void Equal<T>(T expected, T actual)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new InvalidOperationException($"Expected {expected}, got {actual}.");
    }

    public static void Sequence(IReadOnlyList<string> actual, params string[] expected)
    {
        Equal(expected.Length, actual.Count);
        for (var index = 0; index < expected.Length; ++index)
            Equal(expected[index], actual[index]);
    }
}
