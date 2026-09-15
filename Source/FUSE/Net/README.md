# fuse_net — B7.4 Networking

Deterministic rollback and authoritative state-sync scaffolding for Track B7.4. Transport, serializer, rollback buffer, and snapshot delta types are swappable without touching game logic.

## Layout

| Header | Role |
|--------|------|
| `game_state.hpp` | `GameSnapshot` and `PlayerInput` rollback payloads |
| `transport.hpp` | `Transport` abstraction, loopback/ENet/Steam backends, `TransportStats`, factory |
| `serializer.hpp` | Compact binary message serialisation |
| `rollback.hpp` | `RollbackManager` GGPO-style resimulation bound to ECS |
| `rollback_buffer.hpp` | Ring buffer of per-frame snapshots and confirmed inputs |
| `snapshot_delta.hpp` | `SnapshotDelta` / entity patches for bandwidth-friendly sync |
| `state_sync.hpp` | `ClientInterpolator`, `StateSyncDeltaBroadcaster`, entity deltas |

## Rollback buffer

`RollbackBuffer` stores up to 64 frames of `GameSnapshot` plus local/remote `PlayerInput`. `RollbackManager` delegates snapshot/input retention to the buffer so transport/session code can inspect history without duplicating ring logic.

## Snapshot deltas

`compute_snapshot_delta(base, target)` emits `SnapshotDeltaKind::None`, `EntityPatch`, or `Full` depending on how many entity rows changed. `apply_snapshot_delta` reconstructs a target snapshot from a base frame plus delta payload. `StateSyncDeltaBroadcaster` maps authoritative `StateSyncSnapshot` bundles into the same delta path.

## Transport stubs

- **Loopback** — in-process linked peers, channel stats, `UnreliableSeq` newest-wins delivery
- **ENet / Steam** — compile-safe stubs (`init` returns false) with `last_error()` diagnostics

Use `create_transport(TransportBackend)` to instantiate the active backend.

## Tests

`fuse_net_tests` (`ctest` name `fuse_net_b74`) covers loopback channels/stats, serializer round-trip, rollback resimulation, rollback buffer retention, snapshot delta apply/serialize, and client interpolation without ENet or Steam dependencies.
