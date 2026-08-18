namespace Shinkou.Engine.Scripting;

public enum LifecycleState
{
    Constructing,
    Alive,
    Inactive,
    DestroyRequested,
    Destroyed,
}

public readonly record struct ObjectId(ulong Value)
{
    public override string ToString() => Value.ToString();
}

public readonly record struct Entity(uint Id, uint Generation)
{
    public bool IsValid => Id != 0;
}

public abstract class SceneObject
{
    private World? world;
    private LifecycleState state = LifecycleState.Constructing;
    private bool active = true;

    public World World => world ?? throw new InvalidOperationException("The scene object is not attached to a world.");
    public LifecycleState LifecycleState => state;
    public bool Active => active && state is LifecycleState.Alive;

    public void SetActive(bool value)
    {
        if (active == value || state is LifecycleState.DestroyRequested or LifecycleState.Destroyed)
            return;

        active = value;
        if (active)
        {
            state = LifecycleState.Alive;
            OnEnable();
        }
        else
        {
            state = LifecycleState.Inactive;
            OnDisable();
        }
    }

    public void Destroy() => world?.DestroyObject(this);

    protected virtual void OnCreate() { }
    protected virtual void OnEnable() { }
    protected virtual void OnDisable() { }
    protected virtual void OnUpdate(float deltaSeconds) { }
    protected virtual void OnDestroyRequested() { }
    protected virtual void OnDestroy() { }

    internal void Attach(World owner)
    {
        world = owner;
        state = LifecycleState.Alive;
        OnCreate();
        if (active)
            OnEnable();
    }

    internal void Update(float deltaSeconds)
    {
        if (Active)
            OnUpdate(deltaSeconds);
    }

    internal void RequestDestroy()
    {
        if (state is LifecycleState.DestroyRequested or LifecycleState.Destroyed)
            return;

        state = LifecycleState.DestroyRequested;
        OnDestroyRequested();
    }

    internal void Dispose()
    {
        if (state == LifecycleState.Destroyed)
            return;

        if (Active)
        {
            active = false;
            state = LifecycleState.Inactive;
            OnDisable();
        }

        OnDestroy();
        state = LifecycleState.Destroyed;
        world = null;
    }
}

public abstract class ScriptComponent
{
    private GameObject? gameObject;
    private LifecycleState state = LifecycleState.Constructing;
    private bool enabled = true;

    public GameObject GameObject => gameObject ?? throw new InvalidOperationException("The component is not attached to a game object.");
    public World World => GameObject.World;
    public LifecycleState LifecycleState => state;
    internal bool IsAttached => gameObject is not null;
    public bool Enabled
    {
        get => enabled;
        set
        {
            if (enabled == value || state is LifecycleState.DestroyRequested or LifecycleState.Destroyed)
                return;

            enabled = value;
            if (GameObject.ActiveInHierarchy)
            {
                if (enabled) OnEnable();
                else OnDisable();
            }
        }
    }

    protected virtual void OnAttach() { }
    protected virtual void OnCreate() { }
    protected virtual void OnDetach() { }
    protected virtual void OnEnable() { }
    protected virtual void OnDisable() { }
    protected virtual void OnUpdate(float deltaSeconds) { }
    protected virtual void OnDestroyRequested() { }
    protected virtual void OnDestroy() { }

    internal void Attach(GameObject owner)
    {
        gameObject = owner;
        state = LifecycleState.Alive;
        OnAttach();
        OnCreate();
        if (owner.ActiveInHierarchy && enabled)
            OnEnable();
    }

    internal void NotifyActiveState(bool active)
    {
        if (state is LifecycleState.DestroyRequested or LifecycleState.Destroyed)
            return;

        state = active ? LifecycleState.Alive : LifecycleState.Inactive;
        if (!enabled)
            return;

        if (active) OnEnable();
        else OnDisable();
    }

    internal void Update(float deltaSeconds)
    {
        if (state == LifecycleState.Alive && enabled)
            OnUpdate(deltaSeconds);
    }

    internal void RequestDestroy()
    {
        if (state is LifecycleState.DestroyRequested or LifecycleState.Destroyed)
            return;

        state = LifecycleState.DestroyRequested;
        OnDestroyRequested();
    }

    internal void Dispose()
    {
        if (state == LifecycleState.Destroyed)
            return;

        if (state == LifecycleState.Alive && gameObject?.ActiveInHierarchy == true && enabled)
        {
            OnDisable();
        }

        OnDetach();
        OnDestroy();
        state = LifecycleState.Destroyed;
        gameObject = null;
    }
}

public abstract class EcsSystem
{
    private World? world;
    private LifecycleState state = LifecycleState.Constructing;
    private bool enabled = true;

    public World World => world ?? throw new InvalidOperationException("The system is not attached to a world.");
    public LifecycleState LifecycleState => state;
    public bool Enabled
    {
        get => enabled;
        set
        {
            if (enabled == value || state is LifecycleState.DestroyRequested or LifecycleState.Destroyed)
                return;

            enabled = value;
            if (enabled) OnEnable();
            else OnDisable();
        }
    }

    public virtual int Order => 0;

    protected virtual void OnCreate() { }
    protected virtual void OnEnable() { }
    protected virtual void OnDisable() { }
    protected virtual void OnUpdate(float deltaSeconds) { }
    protected virtual void OnDestroyRequested() { }
    protected virtual void OnDestroy() { }

    internal void Attach(World owner)
    {
        world = owner;
        state = LifecycleState.Alive;
        OnCreate();
        if (enabled) OnEnable();
    }

    internal void Update(float deltaSeconds)
    {
        if (enabled && state == LifecycleState.Alive)
            OnUpdate(deltaSeconds);
    }

    internal void Dispose()
    {
        if (state == LifecycleState.Destroyed)
            return;

        if (enabled)
        {
            enabled = false;
            OnDisable();
        }

        OnDestroy();
        state = LifecycleState.Destroyed;
        world = null;
    }
}
