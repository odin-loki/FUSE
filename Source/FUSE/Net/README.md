# fuse_net — B7.4 Networking

Deterministic rollback and authoritative state-sync scaffolding for Track B7.4. Transport, serializer, rollback buffer, and snapshot delta types are swappable without touching game logic.

## Layout

| Header | Role |
|--------|------|
| `game_state.hpp` | `GameSnapshot` and `PlayerInput` rollback payloads |
| `checksum.hpp` | FNV-1a helpers and `compute_snapshot_checksum` / `verify_snapshot_checksum` |
| `input_history.hpp` | 128-frame ring of predicted + confirmed `PlayerInput` |
| `reconcile.hpp` | `reconcile_predicted_input` — compare authoritative vs predicted locals |
| `transport.hpp` | `Transport` abstraction, loopback/ENet/Steam backends, `TransportStats`, factory |
| `serializer.hpp` | Compact binary message serialisation |
| `rollback.hpp` | `RollbackManager` GGPO-style resimulation bound to ECS |
| `rollback_buffer.hpp` | Ring buffer of per-frame snapshots and confirmed inputs |
| `snapshot_delta.hpp` | `SnapshotDelta` / entity patches for bandwidth-friendly sync |
| `state_sync.hpp` | `ClientInterpolator`, `StateSyncDeltaBroadcaster`, entity deltas |
| `interest_management.hpp` | AOI relevance radii, `InterestManager`, `InterestPriorityQueue` |

## Input prediction history

`InputHistoryBuffer` stores up to 128 frames of predicted and confirmed `PlayerInput`. `RollbackManager::set_local_input` records predictions; `apply_remote_input` calls `reconcile_predicted_input` before resimulation. Use `inputs_equal` to compare payloads without frame ids.

## Rollback buffer

`RollbackBuffer` stores up to 64 frames of `GameSnapshot` plus local/remote `PlayerInput`. `RollbackManager` delegates snapshot/input retention to the buffer so transport/session code can inspect history without duplicating ring logic.

## Snapshot deltas

`compute_snapshot_delta(base, target)` emits `SnapshotDeltaKind::None`, `EntityPatch`, or `Full` depending on how many entity rows changed. Entity patches include `SnapshotEcsField` / `SnapshotPhysicsField` masks and a `changed_entity_mask` bitset. `apply_snapshot_delta` reconstructs a target snapshot from a base frame plus delta payload; `apply_snapshot_delta_verified` checks baseline/target checksums. `SnapshotHistoryRing` stores recent snapshots via `RollbackBuffer` and supports `apply_delta_and_store`. `StateSyncDeltaBroadcaster` maps authoritative `StateSyncSnapshot` bundles into the same delta path.

## Transport stubs

- **Loopback** — in-process linked peers, channel stats, `UnreliableSeq` newest-wins delivery
- **ENet / Steam** — compile-safe stubs (`init` returns false) with `last_error()` diagnostics

Use `create_transport(TransportBackend)` to instantiate the active backend.

## Interest management (AOI)

Server-side area-of-interest scaffolding mirrors Torque ghost scoping with FUSE-native relevance radii:

- **`InterestPolicy`** — `relevance_radius`, optional `unload_radius` (default 1.25×), inner `always_relevant_radius`
- **`InterestManager`** — registers entity positions, evaluates scope/priority for an observer
- **`InterestPriorityQueue`** — max-priority heap for replication ordering (closest / always-relevant first)

Hysteresis keeps entities in scope until they pass the unload radius, matching B7.5 terrain streaming semantics.

## Tests

`fuse_net_tests` (`ctest` name `fuse_net_b74`) covers loopback channels/stats, serializer round-trip, rollback resimulation, rollback buffer retention, checksum helpers, input history eviction, reconcile outcomes, snapshot delta apply/serialize, client interpolation, relevance radius classification/hysteresis, and interest priority queue ordering without ENet or Steam dependencies.

See [docs/unification/TRACK-B-NET.md](../../docs/unification/TRACK-B-NET.md) for the B7.4 deepen checklist.
