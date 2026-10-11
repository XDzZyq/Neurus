# Neurus Documentation

Neurus is a C++20 Vulkan-HPP 1.4 real-time renderer built around strict layer
isolation; this page indexes every document in the set.

## Architecture

- [Overview](architecture/overview.md) - design philosophy, layer diagram, current scope
- [Layers](architecture/layers.md) - responsibilities of the renderer, editor, UI, asset, scene and core layers
- [Data Flow](architecture/data-flow.md) - the isolation chain, the three event paths, and context objects
- [Ownership](architecture/ownership.md) - the Vulkan ownership graph
- [Coordinate System](architecture/coordinate-system.md) - the Z-up, +Y forward convention

## Rendering

- [Overview](render/overview.md)
- [Passes](render/passes.md)
- [Render Graph](render/render-graph.md)
- [Buffers](render/buffers.md)
- [Resources](render/resources.md)
- [Shaders](render/shaders.md)
- [Temporal Accumulation](render/temporal.md)

## Editor

- [Overview](editor/overview.md)
- [Viewport](editor/viewport.md)
- [Controllers](editor/controllers.md)
- [Events](editor/events.md)
- [Operations](editor/operations.md)
- [Transform Gizmo](editor/gizmo.md)
- [Debug Overlays](editor/debug-overlays.md)

## UI

- [Overview](ui/overview.md)
- [Panels](ui/panels.md)
- [Items](ui/items.md)
- [Models](ui/models.md)
- [Internationalization](ui/i18n.md)

## Core, Scene and Asset

- [UID and Resources](core/uid-and-resources.md)
- [Serialization](core/serialization.md)
- [Render Config](core/render-config.md)
- [Logging](core/logging.md)
- [Scene Objects](scene/objects.md)
- [Transform](scene/transform.md)
- [Overlays](scene/overlays.md)
- [Debug Objects](scene/debug-objects.md)
- [Project Serialization](asset/project-serialization.md)
- [Data Resources](asset/data-resources.md)

## Style

- [Style](style/style.md) - language, formatting, naming, RAII and includes
- [Conventions](style/conventions.md) - error handling, logging, comments and shader conventions

## Build

- [Build](build/build.md) - prerequisites, presets, the two build trees and commands
- [Test](build/test.md) - test standards, GPU test patterns and reference-image rules
