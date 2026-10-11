# Serialization

Neurus persists projects with cereal, and `core/Serialize.h` provides the version-tolerant helpers that let a field be absent from a file without discarding the whole component around it.

## The version-drift problem

Two builds of the app rarely agree on a field list. An older build opening a newer project, or a newer build opening an older one, hits the same wall: a name the reader asks for is not in the file, cereal reports that by throwing `cereal::Exception`, and the throw unwinds past the whole component's `Load`.

Because the catch blocks are deliberately coarse - `ResourceComponent::Load` calls `ResourceManager::Clear()`, `ConfigComponent::Load` assigns a fresh `RenderConfig` - one unknown field can discard an entire scene or every render setting around it. The cure is to read such a field on its own and let it keep its default when the file has nothing to say about it.

## The three helpers (`core/Serialize.h`)

The namespace is `archive`, not `serialize`: several headers declare free `neurus::serialize(Archive&, T&)` overloads for cereal to find by ADL, and a namespace of that name inside `neurus` would redefine them.

- `archive::Optional(ar, CEREAL_NVP(a), CEREAL_NVP(b), ...)` - the default. Each field is transferred independently, so one absent name does not stop the ones around it. On absence it **leaves the member alone**, and returns how many fields were absent.
- `archive::LoadFresh(ar, "key", live)` - load-side only, for the two sites that load into an object already in use. It reads the named object into a default-constructed temporary and assigns it over the live one, so "absent" means *the in-class default*, not "inherit the last project the user had open". The two sites are `ConfigComponent::Load` and `Preferences::Load`.
- `archive::OptionalBlock(ar, ...)` - all-or-nothing: saving writes the fields and returns `true`, loading reads them and returns `false` if the whole block was missing. Use it when the group genuinely is a unit, for recognising a whole legacy format (the Scene's pre-pool ID lists, its selection block), or when "absent" must stay distinguishable from any value the field can hold (the Scene's three-state `activeCamUid`).

`OptionalField(ar, field)` is the single-field primitive underneath all three. `NEURUS_OPTIONAL(ar, field)` is sugar over `Optional` for the common case where the archive key is spelled exactly like the member.

There is deliberately no `NEURUS_OPTIONAL_NVP(ar, field, fallback)`. It used to exist and was the one construct that let a default be written twice. Absence now means "keep what the constructor set", so a fallback argument has nothing to say. A field that genuinely needs a load-time value different from its constructed one is a migration, not a default: write it as an explicit `if (!Optional(...))` at the call site, where the reason can be stated.

## One definition of every default

A field's default is written once, in its in-class member initializer (or the type's default constructor, for `o_type` and `o_name`). Nothing in a load path restates it, and that is enforced rather than asked for:

- `Optional` on absence leaves the member alone, and has no fallback parameter to pass, so there is no second place a default could be spelled.
- That only holds if the object starts at its defaults, which is free for anything cereal constructs (every pooled scene object). The two load-into-a-live-object sites read into a freshly-constructed instance and assign it over the live one, so the defaults come from that construction.
- `test/asset/test_serialize_optional.cpp` loads an archive with every optional key renamed away and compares the result field-by-field against a default-constructed object, which fails if the two ever disagree.

The same test pins the other direction with a real file: it loads the shipped `res/default.neurus.json` (written before any of this existed, still missing `r_debug_draw`), re-saves it, and asserts the re-saved document kept every member name the original had - gaining keys is allowed, losing one is not.

## Name-keyed archives only

Per-field optionality needs an archive that can report "no such name" without losing its place. Every archive in this project is a `cereal::JSONInputArchive`, whose `search()` scans the current object from its first member, assigns the cursor only on a hit, and clears its pending name before it can throw - so a failed lookup leaves the cursor and the node stack untouched, and **field order does not matter**.

`Optional` `static_assert`s against positional archives, which cannot report absence without desynchronising the stream. Keep the *write* order stable anyway: a reordered save is a needlessly different file, and `Light::serialize` splits its call three ways for no other reason.

An `OptionalField` note: either the whole field is in the file or none of it is. A compound field whose node exists but whose contents are incomplete throws from inside the node, which is a corrupt file rather than merely a different version, and is not what the helper is for.

## What stays strict, and why

Value fields are optional; identity and history are not. Absence there means a broken reference or a replay that would corrupt the scene, which must surface instead of being papered over:

- `UID::o_id` - every cross-reference resolves through it.
- The `cereal::base_class<>` hooks.
- The resource pool itself, and the Scene's ID lists.
- A data resource's source path: `MeshData` / `ImageData::m_path`, `Shader::m_name`, `RenderShader::m_vertPath` / `m_fragPath`.
- `Light::light_type` - defaulting it yields `NONELIGHT`, an invisible light with no error anywhere.
- Every undo/redo `Operation`'s own fields - a half-read op replays a `m_before` nobody ever had. An op's *endpoint contents* do follow the per-field rule, because `SetRenderConfigOp` embeds a `RenderConfig`; only the endpoint node itself is required.

## Version tolerance

What this does and does not buy an *older* build opening a *newer* file:

| Newer file has... | Older build does | Where |
|---|---|---|
| a field this build lacks | ignores it | cereal's `finishNode()` does not check that members were consumed |
| a field renamed since | defaults that one field, keeps the rest | `archive::Optional` |
| a whole component node this build lacks | ignores it | `Project::load` iterates *its* components and finds each by name |
| a component node this build has but the file lacks | defaults it, reports one line | `LoadFresh`'s false branch |
| a pooled object type this build does not know | fails the whole pool | `ResourceComponent::Load` - known limit, left open on purpose |
| an enum value out of this build's range | keeps it as-is | known limit, left open on purpose |

A field the reader never asks for is already harmless: the JSON input archive looks members up by name and never checks that an object's members were all consumed, so a newer file's extra fields and extra top-level components are skipped silently. The helpers add tolerance for a field that was *renamed, moved or removed* between the two builds, in either direction.

Order on disk does not matter on load because the lookup is name-keyed.

## `base_class` adds a node

`cereal::base_class<ObjectID>(this)` writes a `"value0"` object, so a `Mesh`'s `o_id` sits two `"value0"` levels deep (`value0.value0.o_id`) and the mesh's own fields are flat beside `"transform"`. Hand-writing a legacy archive for a test means reproducing that nesting; saving a real object and patching the text is the safer idiom.

## Project and Serializable

`src/asset/Project.h` defines `project::Project`, a pure registration-based serializer that owns no data - no project path, no dirty flag, no concrete type knowledge:

- Components are registered via `Register<T>(args...)`, which constructs the component and stores it as `unique_ptr<Serializable>`, returning a reference.
- `Save(path)` / `Load(path)` iterate every registered component and call its virtual `Save` / `Load`. `Project::Save` writes `cereal::make_nvp("project", *this)` into a `.neurus.json` file.
- `Clear()` destroys all components.
- It depends only on cereal and `Serializable.h` - no scene or render headers.

`src/asset/Serializable.h` defines the component interface `project::Serializable`:

- `Key()` returns the archive node name.
- `Save(cereal::JSONOutputArchive&)` / `Load(cereal::JSONInputArchive&)` are the persistence hooks.

`Project`'s own `save` / `load` cast to the JSON archive because the virtual interface is typed to JSON archives. Registration order is archive read order, so a new component belongs at the end. The concrete components and their order are covered in [Project Serialization](../asset/project-serialization.md). The pool-level registration that makes polymorphic serialization work is covered in [UID and Resources](uid-and-resources.md).
