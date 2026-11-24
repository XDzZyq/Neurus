# Serialization

Neurus persists projects with cereal, and `core/Serialize.hpp` provides the version-tolerant helpers that let a field be absent from a file without discarding the whole component around it.

## The version-drift problem

Two builds of the app rarely agree on a field list. An older build opening a newer project, or a newer build opening an older one, hits the same wall: a name the reader asks for is not in the file, cereal reports that by throwing `cereal::Exception`, and the throw unwinds past the whole component's `Load`.

A catch around a whole component can discard valid neighbors after one field fails.
Per-field helpers handle version drift. Disk project loading additionally isolates
components and resource entries before invoking cereal, so recovery does not depend
on a shared archive cursor.

## The three helpers (`core/Serialize.hpp`)

The namespace is `archive`, not `serialize`: several headers declare free `neurus::serialize(Archive&, T&)` overloads for cereal to find by ADL, and a namespace of that name inside `neurus` would redefine them.

- `archive::Optional(ar, CEREAL_NVP(a), CEREAL_NVP(b), ...)` - the default. Each field is transferred independently, so one absent name does not stop the ones around it. On absence it **leaves the member alone**, and returns how many fields were absent.
- `archive::LoadFresh(ar, "key", live)` - load-side only. It reads into a default-constructed temporary and assigns it over the live value, so missing fields use declared defaults instead of the previous session. Optional components use the return value to retain a null slot when the whole node is absent or unusable.
- `archive::OptionalBlock(ar, ...)` - all-or-nothing: saving writes the fields and returns `true`, loading reads them and returns `false` if the whole block was missing. Scene membership arrays and its selection block use this form so a partial group cannot restore a partial selection or membership list.

`OptionalField(ar, field)` is the single-field primitive underneath all three. `NEURUS_OPTIONAL(ar, field)` is sugar over `Optional` for the common case where the archive key is spelled exactly like the member.

There is deliberately no `NEURUS_OPTIONAL_NVP(ar, field, fallback)`. It used to exist and was the one construct that let a default be written twice. Absence now means "keep what the constructor set", so a fallback argument has nothing to say. A field that genuinely needs a load-time value different from its constructed one is a migration, not a default: write it as an explicit `if (!Optional(...))` at the call site, where the reason can be stated.

## One definition of every default

A field's default is written once, in its in-class member initializer (or the type's default constructor, for `o_type` and `o_name`). Nothing in a load path restates it, and that is enforced rather than asked for:

- `Optional` on absence leaves the member alone, and has no fallback parameter to pass, so there is no second place a default could be spelled.
- That only holds if the object starts at its defaults. Cereal constructs pooled objects; load-into-live-value helpers construct a fresh temporary first. A present config object therefore gets per-field defaults, while an absent config keeps its project pointer null.
- `test/asset/test_serialize_optional.cpp` loads an archive with every optional key renamed away and compares the result field-by-field against a default-constructed object, which fails if the two ever disagree.

The same test pins the other direction with a real file: it loads the shipped `res/default.neurus.json` (written before any of this existed, still missing `r_debug_draw`), re-saves it, and asserts the re-saved document kept every member name the original had - gaining keys is allowed, losing one is not.

## Name-keyed archives only

Per-field optionality needs an archive that can report "no such name" without losing its place. Every archive in this project is a `cereal::JSONInputArchive`, whose `search()` scans the current object from its first member, assigns the cursor only on a hit, and clears its pending name before it can throw - so a failed lookup leaves the cursor and the node stack untouched, and **field order does not matter**.

`Optional` `static_assert`s against positional archives, which cannot report absence without desynchronising the stream. Keep the *write* order stable anyway: a reordered save is a needlessly different file, and `Light::serialize` splits its call three ways for no other reason.

An `OptionalField` note: either the whole field is in the file or none of it is. A compound field whose node exists but whose contents are incomplete throws from inside the node, which is a corrupt file rather than merely a different version, and is not what the helper is for.

## What stays strict, and why

Value fields are optional; identity and history are not. Absence there means a broken reference or a replay that would corrupt the scene, which must surface instead of being papered over:

- The pool record key identifies the object for every cross-reference. A standalone object carries only its fields, and its UID base serializes as an empty object.
- The `cereal::base_class<>` hooks.
- Resource identities and the Scene's ID lists. Individual disk pool entries are
  isolated: a malformed or unknown entry is skipped and reported without losing
  valid entries. Bare cereal pool loading still has its coarse failure behavior.
- Mesh and image source paths, shader names and stage paths are required. Shader
  project keys keep their original spelling, including `m_name`, `m_vertPath`,
  `m_fragPath` and `m_compPath`.
- `Light::light_type` - defaulting it yields `NONELIGHT`, an invisible light with no error anywhere.
- Undo/redo operations require the data needed for replay. Config operations
  store optional endpoint presence: `before_present` / `after_present` default to
  true for older histories; a false flag omits that endpoint object and replay
  restores the null config slot. Present endpoint contents use normal per-field
  defaults, and undo can distinguish absent config from explicitly stored defaults.

## Version tolerance

What this does and does not buy an *older* build opening a *newer* file:

| Newer file has... | Older build does | Where |
|---|---|---|
| a field this build lacks | ignores it | cereal's `finishNode()` does not check that members were consumed |
| a field renamed since | defaults that one field, keeps the rest | `archive::Optional` |
| a whole component node this build lacks | ignores it | disk `Project::Load` selects registered component keys |
| an optional config or graph node the file lacks | keeps its stored slot null; effective reads use defaults | component recovery policy |
| a pooled object type this build does not know | skips and reports that entry, retaining valid neighbors | disk `ResourceComponent::LoadJson` |
| a malformed config field or out-of-range config enum | defaults that field, retaining valid neighbors | disk `ConfigComponent::LoadJson` |
| an out-of-range enum through bare cereal loading | no general range validation | the helpers provide optionality, not domain validation |

A field the reader never asks for is already harmless: the JSON input archive looks members up by name and never checks that an object's members were all consumed, so a newer file's extra fields and extra top-level components are skipped silently. The helpers add tolerance for a field that was *renamed, moved or removed* between the two builds, in either direction.

Order on disk does not matter on load because the lookup is name-keyed.

Disk config loading accepts a present object even when some fields are unusable.
It removes known fields with wrong primitive types or unrepresentable numeric
values, and checks config enum domains, before cereal reads the sanitized object.
Missing, null or non-object config stays absent. Save omits null config, graph and
shader data; rendering with cached built-ins does not insert fallback objects
into the saved project. These disk policies are distinct from the lower-level
`Optional` / `LoadFresh` helpers used directly with cereal.


## `base_class` adds a node

`cereal::base_class<ObjectID>(this)` writes the scene metadata under a `"value0"` object. A mesh's own fields sit beside `"transform"`. Its UID base has no serialized fields. The outer pool record's `key` supplies the identity.

## Project and Serializable

`src/asset/Project.hpp` defines `project::Project`, a pure registration-based serializer that owns no data - no project path, no dirty flag, no concrete type knowledge:

- Components are registered via `Register<T>(args...)`, which constructs the component and stores it as `unique_ptr<Serializable>`, returning a reference.
- `Save(path)` invokes component `Save` hooks through cereal and writes the
  `project` object into a `.neurus.json` file.
- Disk `Load(path)` parses that object with `ordered_json`, selects each registered
  component key and invokes `LoadJson` with an isolated wrapper. A malformed
  component cannot strand the cursor used by another component.
- `Clear()` destroys all components.
- It uses cereal and JSON parsing, with no scene or render implementation headers.

`src/asset/Serializable.hpp` defines the component interface `project::Serializable`:

- `Key()` returns the archive node name.
- `Save(cereal::JSONOutputArchive&)` / `Load(cereal::JSONInputArchive&)` are the
  cereal hooks.
- `LoadJson` is the isolated disk-load boundary. Its default adapter creates a
  local cereal archive; config, graph and resource components can validate or
  isolate their payload before invoking typed loading.

The lower-level `Project::save` / `load` cereal adapters remain available, but
file loading uses the isolated `LoadJson` path above. Registration order still
matters: the resource pool loads before scene references, and graph application
waits until the pool is restored. The concrete components and their order are covered in [Project Serialization](../asset/project-serialization.md). The pool-level registration that makes polymorphic serialization work is covered in [UID and Resources](uid-and-resources.md).
