# ShinkouEngine reflection

ShinkouEngine reflection is a runtime metadata layer for engine and gameplay modules. It is split into a dependency-free core (`Reflection.h`), JSON serialization (`Serialization.h`), and engine registrations (`EngineReflection.h`). It does not depend on editor, renderer, scripting, RTTI, or a global singleton.

## Registration

Use stable fully-qualified names. A custom type used as a field or method argument must have a `TypeName<T>` specialization:

```cpp
SHINKOU_REFLECT_TYPE(MyComponent, "game.MyComponent");

auto descriptor = shinkou::reflection::TypeBuilder<MyComponent>("game.MyComponent",
    shinkou::reflection::TypeKind::Object,
    shinkou::reflection::TypeFlags::Serializable)
    .field("health", &MyComponent::health)
    .method<&MyComponent::reset>("reset")
    .take();

registry.register_module("game", 1, {std::move(descriptor)});
registry.freeze();
```

Modules can contain types in any order. `freeze()` validates duplicate IDs/names, member hash collisions, field and method types, container callbacks, base dependencies, multiple-inheritance casts, and inheritance cycles. Registration is rejected after freezing.

## Supported metadata

- Stable FNV-1a `TypeId` and precomputed member-name indexes.
- Primitive, struct, object, enum, opaque, and abstract type kinds.
- Single and multiple inheritance with real pointer adjustment callbacks.
- Fields with read-only/transient/hidden/editor-only flags and arbitrary attributes.
- Member methods with const/noexcept support, typed arguments, return values, and exception isolation.
- Enum name/value lookup in both directions.
- `std::vector<T>` container descriptors through `make_vector_descriptor<T>()`.
- Aligned RAII construction through `ReflectedInstance`.
- Owner-aware inherited field access and method invocation through `FieldDescriptor::address_of(registry, ...)` and `TypeRegistry::invoke(...)`.
- Built-in math/string descriptors and `register_engine_types()` for core ShinkouEngine components.

## Serialization

`serialize_json()` and `deserialize_json()` support primitives, strings, enums, reflected objects, inherited fields, math structs, and registered vector containers. Options include pretty output, transient-field inclusion, unknown-field rejection, and maximum recursion depth. The parser rejects malformed JSON, non-finite numbers, range errors, and invalid enum values.

## Threading and performance

Registration is exclusive and intended for startup/module loading. Frozen lookup is shared-lock read-only; descriptor storage is immutable and pointer-stable for the registry lifetime. Type lookup by ID and name is hash-based, and field/method/enum lookup uses precomputed indexes. Serialization is deliberately allocation-friendly and off the frame hot path; runtime metadata lookup and direct field access do not allocate.

The API intentionally exposes raw addresses only through registered member pointers. It never guesses object layout and never uses `typeid().hash_code()` as a cross-module identity.
