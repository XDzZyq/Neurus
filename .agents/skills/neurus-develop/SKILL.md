---
name: neurus-develop
description: Implement and review Neurus changes that affect layer boundaries, controller edits and undo, frame publication, or GPU uploads.
---

# Neurus development

Start with the [repository rules](../../../AGENTS.md). Before choosing an API
or adding an include, identify which component owns the state, who may change
it and how the caller reaches that owner. Trace a comparable implementation
and use the relevant contract below.

- **Cross-layer changes.** Use the [communication table](../../instructions/architecture.instructions.md#communication)
  to choose the handoff. For example, Editor posts GPU work through
  `RenderRequestSink`, while Application sends window controls to `RenderThread`.
- **Edits and undo.** A controller handles the typed event and submits a
  fine-grained operation containing the target UID and the values needed for
  undo. Keep handlers and concrete dependencies in the implementation file.
  The operation system owns `OperationContext` and replay through `Apply`.
  Follow the [controller constraints](../../instructions/architecture.instructions.md#controllers-and-undo).
- **GPU uploads.** Trace Mesh for a single resource or RenderGraph for ordered
  dependencies before implementing an upload. Identify the queued payload's
  lifetime, posting order and renderer-side cache owner.
  Follow the [upload reference paths](../../../docs/render/resources.md#upload-reference-paths).

For verification, use the [development workflow](../../instructions/development.instructions.md)
and [agent roles](../../instructions/roles.instructions.md). When an
include crosses a layer, the lead agent runs `python scripts/check_layer_boundaries.py`.
