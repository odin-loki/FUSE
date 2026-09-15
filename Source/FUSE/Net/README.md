# fuse_net — B7.4 Networking

Deterministic rollback and authoritative state-sync scaffolding for Track B7.4. Transport, serializer, rollback buffer, and snapshot delta types are swappable without touching game logic.

## Layout

| Header | Role |
|--------|------|
| `game_state.hpp` | `GameSnapshot` and `PlayerInput` rollback payloads |
| `checksum.hpp` | FNV-1a helpers and `compute_snapshot_checksum` / `verify_snapshot_checksum` |
| `input_history.hpp` | 128-frame ring with `push_frame` / `pop_oldest`, predicted + confirmed `PlayerInput` |
| `reconcile.hpp` | `reconcile_predicted_input` — compare authoritative vs predicted locals |
| `rollback_window.hpp` | `can_rewind_to_frame`, `resimulate_frame_count` rollback bounds stubs |
| `transport.hpp` | `Transport` abstraction, loopback/ENet/Steam backends, `TransportStats`, factory |
| `serializer.hpp` | Compact binary message serialisation |
| `rollback.hpp` | `RollbackManager` GGPO-style resimulation bound to ECS |
| `rollback_buffer.hpp` | Ring buffer of per-frame snapshots and confirmed inputs |
| `snapshot_delta.hpp` | `SnapshotDelta` / entity patches for bandwidth-friendly sync |
| `state_sync.hpp` | `ClientInterpolator`, `StateSyncDeltaBroadcaster`, entity deltas |
| `interest_management.hpp` | AOI relevance radii, `InterestManager`, `InterestPriorityQueue`, enter/leave scope diff |

## Input prediction history

`InputHistoryBuffer` stores up to 128 frames of predicted and confirmed `PlayerInput`. Use `push_frame` for local prediction, `pop_oldest` to evict the oldest retained frame, and `reconcile_authoritative` when authoritative input arrives. `RollbackManager::set_local_input` records predictions; `apply_remote_input` calls `reconcile_predicted_input` before resimulation. `can_rewind_to_frame` / `resimulate_frame_count` bound rollback depth; `RollbackManager::rewind_to` restores a stored snapshot without resimulating forward.

## Rollback buffer

`RollbackBuffer` stores up to 64 frames of `GameSnapshot` plus local/remote `PlayerInput`. `RollbackManager` delegates snapshot/input retention to the buffer so transport/session code can inspect history without duplicating ring logic.

## Snapshot deltas

`compute_snapshot_delta(base, target)` emits `SnapshotDeltaKind::None`, `EntityPatch`, or `Full` depending on how many entity rows changed. Entity patches include `SnapshotEcsField` / `SnapshotPhysicsField` masks and a `changed_entity_mask` bitset validated by `validate_changed_entity_mask`. `apply_snapshot_delta` reconstructs a target snapshot from a base frame plus delta payload; `apply_snapshot_delta_verified` checks baseline/target checksums and entity-mask consistency. `SnapshotHistoryRing` stores recent snapshots via `RollbackBuffer`, exposes `stored_frame_count`, and supports wrap-safe `apply_delta_and_store`. `StateSyncDeltaBroadcaster` maps authoritative `StateSyncSnapshot` bundles into the same delta path.

## Transport stubs

- **Loopback** — in-process linked peers, channel stats, `UnreliableSeq` newest-wins delivery
- **ENet / Steam** — compile-safe stubs (`init` returns false) with `last_error()` diagnostics

Use `create_transport(TransportBackend)` to instantiate the active backend.

## Interest management (AOI)

Server-side area-of-interest scaffolding mirrors Torque ghost scoping with FUSE-native relevance radii:

- **`InterestPolicy`** — `relevance_radius`, optional `unload_radius` (default 1.25×), inner `always_relevant_radius`
- **`InterestManager`** — registers entity positions, evaluates scope/priority for an observer
- **`InterestPriorityQueue`** — max-priority heap for replication ordering (closest / always-relevant first)
- **`InterestScopeSet` / `InterestSetDiff`** — enter/leave entity sets between consecutive AOI evaluations
- **`filter_candidates_in_radius`** — radius filter stub over candidate lists (no hysteresis)

Hysteresis keeps entities in scope until they pass the unload radius, matching B7.5 terrain streaming semantics.

## Tests

`fuse_net_tests` (`ctest` name `fuse_net_b74`) covers loopback channels/stats, serializer round-trip, rollback resimulation, rollback window rewind bounds, rollback buffer retention, checksum helpers, input history push/pop eviction, reconcile outcomes, snapshot delta apply/serialize, client interpolation, relevance radius classification/hysteresis, radius filter stubs, enter/leave scope diff, and interest priority queue ordering (empty drain + tie-break) without ENet or Steam dependencies.

See [docs/unification/TRACK-B-NET.md](../../docs/unification/TRACK-B-NET.md) for the B7.4 deepen checklist.
