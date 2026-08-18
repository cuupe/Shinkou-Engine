using System.Numerics;

namespace Shinkou.Engine.Scripting;

public readonly record struct GameObjectHandle(World World, ObjectId Id)
{
    public GameObject? Get() => World.FindObject(Id);
    public bool Valid => Get() is not null;
}

public sealed class GameObject
{
    private readonly World world;
    private readonly List<ScriptComponent> components = new();
    private readonly Dictionary<Type, ScriptComponent> componentLookup = new();
    private readonly List<GameObject> children = new();
    private GameObject? parent;
    private bool activeSelf = true;
    private bool activeInHierarchy;
    private bool destroyRequested;
    private LifecycleState state = LifecycleState.Constructing;

    internal GameObject(World owner, ObjectId objectId, string objectName, GameObject? objectParent)
    {
        world = owner;
        Id = objectId;
        Name = objectName;
        parent = objectParent;
    }

    public ObjectId Id { get; }
    public World World => world;
    public GameObjectHandle Handle => new(world, Id);
    public string Name { get; set; }
    public GameObject? Parent => parent;
    public IReadOnlyList<GameObject> Children => children;
    public bool ActiveSelf => activeSelf;
    public bool ActiveInHierarchy => activeInHierarchy;
    public bool DestroyRequested => destroyRequested;
    public LifecycleState LifecycleState => state;
    public bool HasEcsEntity => EcsEntity.IsValid;
    public Entity EcsEntity { get; private set; }
    public TransformComponent Transform => GetComponent<TransformComponent>()
        ?? throw new InvalidOperationException("Every world game object must have a TransformComponent.");

    public event Action<GameObject>? Created;
    public event Action<GameObject>? DestroyRequestedEvent;
    public event Action<GameObject>? Destroyed;

    public void SetActive(bool value)
    {
        if (activeSelf == value || destroyRequested)
            return;
        activeSelf = value;
        RefreshActiveState(parent?.activeInHierarchy ?? true);
    }

    public void Destroy() => world.DestroyObject(this);

    public GameObject CreateChild(string name = "") => world.CreateObject(name, this);

    public void SetParent(GameObject? nextParent)
    {
        if (nextParent == this || parent == nextParent || (nextParent is not null && nextParent.world != world))
            return;
        for (var ancestor = nextParent; ancestor is not null; ancestor = ancestor.parent)
            if (ancestor == this) return;

        if (parent is not null) parent.RemoveChild(this);
        else world.RemoveRoot(this);
        parent = nextParent;
        if (parent is not null) parent.AddChild(this);
        else world.AddRoot(this);
        RefreshActiveState(parent?.activeInHierarchy ?? true);
    }

    public T AddComponent<T>() where T : ScriptComponent, new()
    {
        if (componentLookup.ContainsKey(typeof(T)))
            throw new InvalidOperationException($"GameObject '{Name}' already has {typeof(T).Name}.");
        var component = new T();
        components.Add(component);
        componentLookup.Add(typeof(T), component);
        component.Attach(this);
        return component;
    }

    public ScriptComponent? AddComponent(string registeredName)
    {
        var component = world.CreateComponent(registeredName);
        return component is null ? null : AddComponent(component);
    }

    public ScriptComponent AddComponent(ScriptComponent component)
    {
        ArgumentNullException.ThrowIfNull(component);
        if (component.IsAttached)
            throw new InvalidOperationException("A script component can only be attached once.");
        if (componentLookup.ContainsKey(component.GetType()))
            throw new InvalidOperationException($"GameObject '{Name}' already has {component.GetType().Name}.");
        components.Add(component);
        componentLookup.Add(component.GetType(), component);
        component.Attach(this);
        return component;
    }

    public T? GetComponent<T>() where T : ScriptComponent =>
        componentLookup.TryGetValue(typeof(T), out var component) ? (T)component : null;

    public bool RemoveComponent<T>() where T : ScriptComponent
    {
        if (!componentLookup.TryGetValue(typeof(T), out var component))
            return false;
        if (activeInHierarchy && component.Enabled)
            component.NotifyActiveState(false);
        component.Dispose();
        componentLookup.Remove(typeof(T));
        components.Remove(component);
        return true;
    }

    public Entity EnableEcs()
    {
        if (destroyRequested)
            return default;
        if (!EcsEntity.IsValid)
            EcsEntity = world.Ecs.Create();
        return EcsEntity;
    }

    public void DisableEcs()
    {
        if (!EcsEntity.IsValid)
            return;
        world.Ecs.Destroy(EcsEntity);
        EcsEntity = default;
    }

    public T AddEcsComponent<T>(T value) where T : notnull
    {
        if (!EcsEntity.IsValid)
            throw new InvalidOperationException("Enable ECS on the game object before adding ECS components.");
        return world.Ecs.Add(EcsEntity, value);
    }

    public T AddEcsComponent<T>() where T : notnull, new() => AddEcsComponent(new T());

    public T? GetEcsComponent<T>() where T : notnull =>
        EcsEntity.IsValid ? world.Ecs.TryGet<T>(EcsEntity) : default;

    public bool RemoveEcsComponent<T>() where T : notnull =>
        EcsEntity.IsValid && world.Ecs.Remove<T>(EcsEntity);

    internal void Initialize()
    {
        AddComponent<TransformComponent>();
        RefreshActiveState(parent?.activeInHierarchy ?? true);
        state = activeInHierarchy ? LifecycleState.Alive : LifecycleState.Inactive;
        Created?.Invoke(this);
    }

    internal void AddChild(GameObject child) => children.Add(child);
    internal void RemoveChild(GameObject child) => children.Remove(child);

    internal void UpdateRecursive(float deltaSeconds, Matrix4x4 parentWorld)
    {
        if (!activeInHierarchy || destroyRequested)
            return;
        Transform.UpdateWorld(parentWorld);
        foreach (var component in components.ToArray())
            if (component.LifecycleState != LifecycleState.DestroyRequested)
                component.Update(deltaSeconds);
        foreach (var child in children.ToArray())
            child.UpdateRecursive(deltaSeconds, Transform.WorldMatrix);
    }

    internal void RequestDestroyRecursive()
    {
        if (destroyRequested)
            return;
        destroyRequested = true;
        state = LifecycleState.DestroyRequested;
        DestroyRequestedEvent?.Invoke(this);
        foreach (var component in components)
            component.RequestDestroy();
        foreach (var child in children)
            child.RequestDestroyRecursive();
        RefreshActiveState(parent?.activeInHierarchy ?? true);
    }

    internal void DisposeRecursive()
    {
        foreach (var child in children.ToArray())
            child.DisposeRecursive();
        children.Clear();
        DisableEcs();
        foreach (var component in components.ToArray())
            component.Dispose();
        components.Clear();
        componentLookup.Clear();
        state = LifecycleState.Destroyed;
        Destroyed?.Invoke(this);
    }

    private void RefreshActiveState(bool parentActive)
    {
        var next = parentActive && activeSelf && !destroyRequested;
        if (next != activeInHierarchy)
        {
            activeInHierarchy = next;
            state = next ? LifecycleState.Alive : LifecycleState.Inactive;
            foreach (var component in components)
                component.NotifyActiveState(next);
        }
        foreach (var child in children)
            child.RefreshActiveState(activeInHierarchy);
    }
}

public sealed class TransformComponent : ScriptComponent
{
    private bool dirty = true;

    private Vector3 position;
    private Quaternion rotation = Quaternion.Identity;
    private Vector3 scale = Vector3.One;

    public Vector3 Position { get => position; set { position = value; dirty = true; } }
    public Quaternion Rotation { get => rotation; set { rotation = value; dirty = true; } }
    public Vector3 Scale { get => scale; set { scale = value; dirty = true; } }
    public Matrix4x4 WorldMatrix { get; private set; } = Matrix4x4.Identity;

    public void MarkDirty() => dirty = true;

    internal void UpdateWorld(Matrix4x4 parentWorld)
    {
        if (!dirty)
            return;
        var local = Matrix4x4.CreateScale(Scale) * Matrix4x4.CreateFromQuaternion(Rotation) *
                    Matrix4x4.CreateTranslation(Position);
        WorldMatrix = local * parentWorld;
        dirty = false;
    }
}

public sealed class World : IDisposable
{
    private readonly List<GameObject> roots = new();
    private readonly Dictionary<ObjectId, GameObject> objectLookup = new();
    private readonly List<SceneObject> sceneObjects = new();
    private readonly List<EcsSystem> systems = new();
    private readonly Dictionary<string, Func<ScriptComponent>> componentFactories = new(StringComparer.Ordinal);
    private readonly List<ScriptAssembly> scriptAssemblies = new();
    private ulong nextObjectId = 1;
    private bool updating;

    public World()
    {
        RegisterComponent<TransformComponent>("Transform");
    }

    public EcsRegistry Ecs { get; } = new();
    public int ObjectCount => objectLookup.Count;
    public IReadOnlyList<GameObject> RootObjects => roots;

    public void RegisterComponent<T>(string name) where T : ScriptComponent, new()
    {
        if (string.IsNullOrWhiteSpace(name))
            throw new ArgumentException("A component name is required.", nameof(name));
        RegisterComponent(name, static () => new T());
    }

    public void RegisterComponent(string name, Func<ScriptComponent> factory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentNullException.ThrowIfNull(factory);
        if (!componentFactories.TryAdd(name, factory))
            throw new InvalidOperationException($"Component name '{name}' is already registered.");
    }

    public void MountScriptAssembly(ScriptAssembly assembly)
    {
        ArgumentNullException.ThrowIfNull(assembly);
        foreach (var type in assembly.ComponentTypes)
        {
            var typeName = type.FullName ?? type.Name;
            RegisterComponent(typeName, () => assembly.CreateComponent(typeName));
        }
        scriptAssemblies.Add(assembly);
    }

    public GameObject CreateObject(string name = "", GameObject? parent = null)
    {
        if (parent is not null && parent.World != this)
            throw new InvalidOperationException("The parent belongs to another world.");
        var gameObject = new GameObject(this, new ObjectId(nextObjectId++), name, parent);
        objectLookup.Add(gameObject.Id, gameObject);
        if (parent is null) roots.Add(gameObject);
        else parent.AddChild(gameObject);
        gameObject.Initialize();
        return gameObject;
    }

    public T AddObject<T>() where T : SceneObject, new()
    {
        var sceneObject = new T();
        sceneObjects.Add(sceneObject);
        sceneObject.Attach(this);
        return sceneObject;
    }

    public T AddSystem<T>() where T : EcsSystem, new()
    {
        var system = new T();
        AddSystem(system);
        return system;
    }

    public void AddSystem(EcsSystem system)
    {
        ArgumentNullException.ThrowIfNull(system);
        systems.Add(system);
        systems.Sort(static (left, right) => left.Order.CompareTo(right.Order));
        system.Attach(this);
    }

    public GameObject? FindObject(ObjectId id) => objectLookup.TryGetValue(id, out var value) ? value : null;

    public void DestroyObject(GameObject gameObject)
    {
        if (!ReferenceEquals(gameObject.World, this))
            return;
        gameObject.RequestDestroyRecursive();
        if (!updating) CollectDestroyed();
    }

    public void DestroyObject(SceneObject sceneObject)
    {
        if (!sceneObjects.Contains(sceneObject))
            return;
        sceneObject.RequestDestroy();
        if (!updating)
        {
            sceneObject.Dispose();
            sceneObjects.Remove(sceneObject);
        }
    }

    public void Update(float deltaSeconds)
    {
        if (deltaSeconds < 0) deltaSeconds = 0;
        updating = true;
        foreach (var sceneObject in sceneObjects.ToArray())
            sceneObject.Update(deltaSeconds);
        foreach (var root in roots.ToArray())
            root.UpdateRecursive(deltaSeconds, Matrix4x4.Identity);
        foreach (var system in systems.ToArray())
            system.Update(deltaSeconds);
        updating = false;
        CollectDestroyed();
    }

    public void ClearSystems()
    {
        foreach (var system in systems.ToArray())
            system.Dispose();
        systems.Clear();
    }

    public void Dispose()
    {
        ClearSystems();
        foreach (var root in roots.ToArray())
            root.RequestDestroyRecursive();
        foreach (var root in roots.ToArray())
        {
            root.DisposeRecursive();
            roots.Remove(root);
        }
        objectLookup.Clear();
        Ecs.Clear();
        foreach (var sceneObject in sceneObjects.ToArray())
            sceneObject.Dispose();
        sceneObjects.Clear();
        foreach (var assembly in scriptAssemblies)
            assembly.Dispose();
        scriptAssemblies.Clear();
    }

    internal ScriptComponent? CreateComponent(string name) =>
        componentFactories.TryGetValue(name, out var factory) ? factory() : null;
    internal void AddRoot(GameObject gameObject) => roots.Add(gameObject);
    internal void RemoveRoot(GameObject gameObject) => roots.Remove(gameObject);

    private void CollectDestroyed()
    {
        foreach (var root in roots.ToArray())
        {
            if (root.DestroyRequested)
            {
                RemoveLookupRecursive(root);
                root.DisposeRecursive();
                roots.Remove(root);
            }
            else
            {
                CollectDestroyedChildren(root);
            }
        }

        for (var index = sceneObjects.Count - 1; index >= 0; --index)
        {
            if (sceneObjects[index].LifecycleState != LifecycleState.DestroyRequested)
                continue;
            sceneObjects[index].Dispose();
            sceneObjects.RemoveAt(index);
        }
    }

    private void CollectDestroyedChildren(GameObject parent)
    {
        foreach (var child in parent.Children.ToArray())
        {
            if (child.DestroyRequested)
            {
                RemoveLookupRecursive(child);
                child.DisposeRecursive();
                parent.RemoveChild(child);
            }
            else
            {
                CollectDestroyedChildren(child);
            }
        }
    }

    private void RemoveLookupRecursive(GameObject gameObject)
    {
        objectLookup.Remove(gameObject.Id);
        foreach (var child in gameObject.Children)
            RemoveLookupRecursive(child);
    }
}
