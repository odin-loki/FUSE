# Track B — B7.4 Networking (deepen)

**Status:** Snapshot delta field masks, verified apply path, snapshot history ring, AOI stubs, input prediction history, reconcile stub, and shared checksum helpers on `fuse_net`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.4  
**Depends on:** B3 ECS (`fuse_ecs`), initial B7.4 transport/rollback scaffolding

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `InterestManager` | `interest_management.hpp/.cpp` | Relevance/unload radii, hysteresis, observer AOI evaluation |
| `InterestPriorityQueue` | `interest_management.hpp/.cpp` | Max-priority replication ordering stub |
| `InputHistoryBuffer` | `input_history.hpp/.cpp` | 128-frame ring with `push_frame` / `pop_oldest`, predicted + confirmed `PlayerInput` |
| `reconcile_predicted_input` | `reconcile.hpp/.cpp` | Compare authoritative input against local prediction |
| Rollback window helpers | `rollback_window.hpp/.cpp` | `can_rewind_to_frame`, `resimulate_frame_count` bounds stubs |
| Checksum helpers | `checksum.hpp/.cpp` | FNV-1a over snapshot blobs; shared by rollback + delta paths |
| `RollbackBuffer` | `rollback_buffer.hpp/.cpp` | 64-frame snapshot + input ring (unchanged capacity) |
| `SnapshotHistoryRing` | `snapshot_delta.hpp/.cpp` | Snapshot-only ring delegating to `RollbackBuffer`; `apply_delta_and_store` |
| `RollbackManager` | `rollback.hpp/.cpp` | Records predicted locals; reconciles on remote apply |
| Snapshot deltas | `snapshot_delta.hpp/.cpp` | Field masks, entity bitsets, encode/decode, verified apply + delta checksum |
| Transport | `transport.hpp` | Loopback + ENet/Steam stubs |

**Not in scope (follow-up PRs):** ENet process-pair smoke, session matchmaking, desync telemetry UI, full GGPO input delay, frustum/LOS refinement of AOI.

---

## Interest management (AOI)

Torque ghost scoping maps to FUSE relevance radii with unload hysteresis (same 1.25× default as B7.5 terrain streaming):

```cpp
#include <fuse/net/interest_management.hpp>

fuse::net::InterestPolicy policy{};
policy.relevance_radius = 128.f;
policy.always_relevant_radius = 8.f;

fuse::net::InterestManager manager;
manager.set_policy(policy);
manager.set_observer_position({0.f, 0.f, 0.f, 0.f});
manager.register_entity({entity_id, {40.f, 0.f, 0.f, 0.f}, 0.f});
manager.evaluate();

fuse::net::InterestPriorityQueue queue;
queue.build_from_manager(manager);

fuse::net::InterestEntry next{};
while (queue.pop(next)) {
    // replicate `next.entity` in priority order
}
```

`classify_interest` and `compute_relevance_priority` are exposed for unit tests and future ghost managers.

---

## Input prediction history

Clients predict with local input before authoritative packets arrive. `InputHistoryBuffer` stores both sides per frame and exposes ring push/pop helpers:

```cpp
#include <fuse/net/input_history.hpp>

fuse::net::InputHistoryBuffer history;
history.init(128);

fuse::net::PlayerInput local{};
local.frame = frame;
local.axis_lx = stick_x;
history.push_frame(frame, local);

// later, when authoritative input arrives:
fuse::net::ReconcileResult result = history.reconcile_authoritative(frame, authoritative);
// result.action == Confirmed | Mismatch | NoOp

// explicit eviction (tests / tooling):
const std::optional<fuse::net::InputHistoryFrame> oldest = history.pop_oldest();
```

`RollbackManager::set_local_input` mirrors predictions into the attached history buffer. `apply_remote_input` records confirmed remotes via the same reconcile helper before resimulation.

### Rollback window helpers

Pure bounds helpers and `RollbackManager` rewind stubs keep rollback depth explicit:

```cpp
#include <fuse/net/rollback_window.hpp>
#include <fuse/net/rollback.hpp>

if (fuse::net::can_rewind_to_frame(current, target, max_rollback)) {
    const fuse::u32 steps = fuse::net::resimulate_frame_count(target, current);
    // ...
}

rollback.can_rewind_to(target_frame);
rollback.rewind_to(target_frame);          // restore snapshot only
rollback.resimulate_count_to(target_frame);  // forward gap stub
```

---

## Checksum helpers

Snapshot checksums use shared FNV-1a helpers so rollback capture and delta verification stay aligned:

```cpp
#include <fuse/net/checksum.hpp>

snapshot.checksum = fuse::net::compute_snapshot_checksum(snapshot);
const bool ok = fuse::net::verify_snapshot_checksum(snapshot);
```

---

## Snapshot delta / state-sync deepen

Entity patches carry per-component field masks (`SnapshotEcsField`, `SnapshotPhysicsField`) and a `changed_entity_mask` bitset (stub: up to 64 entity indices). `compute_snapshot_delta` compares baseline vs current snapshots; `apply_snapshot_delta_verified` checks baseline checksum before reconstructing the target frame.

```cpp
#include <fuse/net/snapshot_delta.hpp>

const fuse::net::SnapshotDelta delta =
    fuse::net::compute_snapshot_delta(baseline, current);

fuse::net::SnapshotHistoryRing history;
history.init(64);
history.push(baseline);

fuse::net::GameSnapshot reconstructed{};
history.apply_delta_and_store(baseline.frame, delta, &reconstructed);

const fuse::net::DeltaApplyResult verified =
    fuse::net::apply_snapshot_delta_verified(baseline, delta);
// verified.base_checksum_ok / target_checksum_ok
```

Wire encode/decode preserves masks and entity bitsets. `compute_delta_checksum` hashes the delta payload for roundtrip diagnostics.

---

## Build

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build --target fuse_net_tests
ctest --test-dir build --output-on-failure -R fuse_net_b74
```

---

## Tests

| Check | Validates |
|-------|-----------|
| `test_net_interest_management` | Relevance/unload radii, hysteresis, priority computation, queue ordering |
| `test_net_checksum` | FNV-1a determinism, combine, snapshot verify |
| `test_net_input_history` | `push_frame` / `pop_oldest`, ring wrap eviction, `inputs_equal` |
| `test_net_reconcile` | Confirmed / mismatch / NoOp reconcile paths, `reconcile_authoritative` |
| `test_net_rollback_window` | Rewind bounds, `resimulate_frame_count`, `RollbackManager::rewind_to` |
| `test_net_snapshot_delta` | Empty delta, single-field mask, multi-entity bitset, wire roundtrip, checksum mismatch, history ring |
| `fuse_net_b74` (umbrella) | Transport, serializer, rollback, buffer, delta, interpolation, AOI |

---

## Gates (B7.4 deepen)

- [x] Interest management AOI stubs (`InterestManager`, relevance radii, priority queue)
- [x] Expanded input history ring (128 frames, `push_frame` / `pop_oldest`, predicted + confirmed)
- [x] Reconcile stub (`reconcile_predicted_input`, `InputHistoryBuffer::reconcile_authoritative`)
- [x] Rollback window helpers (`can_rewind_to_frame`, `resimulate_frame_count`, `RollbackManager::rewind_to`)
- [x] Shared checksum helpers + tests
- [x] Snapshot delta field masks, entity bitsets, verified apply + delta checksum
- [x] `SnapshotHistoryRing` reuses `RollbackBuffer` for rollback-friendly snapshot history
- [x] `RollbackManager` records predictions and reconciles remotes
- [ ] ENet / Steam real backends (follow-up)
- [ ] Process-pair net smoke (follow-up, see [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md))

---

## Related docs

- [Source/FUSE/Net/README.md](../../Source/FUSE/Net/README.md) — module layout
- [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md) — B7.1–B7.10 integration checklist
