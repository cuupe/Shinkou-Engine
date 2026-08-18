namespace Shinkou.Engine.Scripting;

public sealed class EcsRegistry
{
    private interface IComponentStore
    {
        void Remove(Entity entity);
        void Clear();
    }

    private sealed class ComponentStore<T> : IComponentStore where T : notnull
    {
        public readonly Dictionary<Entity, T> Values = new();
        public void Remove(Entity entity) => Values.Remove(entity);
        public void Clear() => Values.Clear();
    }

    private readonly Dictionary<Type, IComponentStore> stores = new();
    private readonly List<uint> generations = new() { 0 };
    private readonly Queue<uint> freeIds = new();
    private readonly HashSet<Entity> alive = new();

    public Entity Create()
    {
        uint id;
        if (freeIds.Count > 0)
        {
            id = freeIds.Dequeue();
        }
        else
        {
            id = checked((uint)generations.Count);
            generations.Add(1);
        }

        var entity = new Entity(id, generations[(int)id]);
        alive.Add(entity);
        return entity;
    }

    public bool Valid(Entity entity) => entity.IsValid && alive.Contains(entity);

    public void Destroy(Entity entity)
    {
        if (!alive.Remove(entity))
            return;

        foreach (var store in stores.Values)
            store.Remove(entity);

        generations[(int)entity.Id] = checked(entity.Generation + 1);
        freeIds.Enqueue(entity.Id);
    }

    public void Clear()
    {
        foreach (var store in stores.Values)
            store.Clear();
        alive.Clear();
        freeIds.Clear();
        for (var id = 1; id < generations.Count; ++id)
            freeIds.Enqueue((uint)id);
    }

    public bool Has<T>(Entity entity) where T : notnull => TryGet<T>(entity) is not null;

    public T Add<T>(Entity entity, T value) where T : notnull
    {
        EnsureValid(entity);
        var store = GetStore<T>();
        store.Values[entity] = value;
        return value;
    }

    public T Emplace<T>(Entity entity) where T : notnull, new() => Add(entity, new T());

    public bool Remove<T>(Entity entity) where T : notnull
    {
        return stores.TryGetValue(typeof(T), out var raw) && ((ComponentStore<T>)raw).Values.Remove(entity);
    }

    public T Get<T>(Entity entity) where T : notnull
    {
        EnsureValid(entity);
        return GetStore<T>().Values[entity];
    }

    public T? TryGet<T>(Entity entity) where T : notnull
    {
        if (!Valid(entity) || !stores.TryGetValue(typeof(T), out var raw))
            return default;
        return ((ComponentStore<T>)raw).Values.TryGetValue(entity, out var value) ? value : default;
    }

    public IEnumerable<(Entity Entity, T Component)> Query<T>() where T : notnull
    {
        if (!stores.TryGetValue(typeof(T), out var raw))
            return Array.Empty<(Entity, T)>();
        return ((ComponentStore<T>)raw).Values
            .Where(pair => Valid(pair.Key))
            .Select(pair => (pair.Key, pair.Value));
    }

    public IEnumerable<(Entity Entity, TFirst First, TSecond Second)> Query<TFirst, TSecond>()
        where TFirst : notnull where TSecond : notnull
    {
        foreach (var (entity, first) in Query<TFirst>())
        {
            var second = TryGet<TSecond>(entity);
            if (second is not null)
                yield return (entity, first, second);
        }
    }

    private ComponentStore<T> GetStore<T>() where T : notnull
    {
        if (!stores.TryGetValue(typeof(T), out var raw))
        {
            var created = new ComponentStore<T>();
            stores.Add(typeof(T), created);
            return created;
        }

        return (ComponentStore<T>)raw;
    }

    private void EnsureValid(Entity entity)
    {
        if (!Valid(entity))
            throw new InvalidOperationException($"Entity {entity} is not valid.");
    }
}
