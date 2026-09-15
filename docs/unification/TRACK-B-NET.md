# Track B — B7.4 Networking (deepen)

**Status:** Interest management AOI stubs, input prediction history, reconcile stub, and shared checksum helpers on `fuse_net`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.4  
**Depends on:** B3 ECS (`fuse_ecs`), initial B7.4 transport/rollback scaffolding

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `InterestManager` | `interest_management.hpp/.cpp` | Relevance/unload radii, hysteresis, observer AOI evaluation |
| `InterestPriorityQueue` | `interest_management.hpp/.cpp` | Max-priority replication ordering stub |
| `InputHistoryBuffer` | `input_history.hpp/.cpp` | 128-frame ring of predicted + confirmed `PlayerInput` |
| `reconcile_predicted_input` | `reconcile.hpp/.cpp` | Compare authoritative input against local prediction |
| Checksum helpers | `checksum.hpp/.cpp` | FNV-1a over snapshot blobs; shared by rollback + delta paths |
| `RollbackBuffer` | `rollback_buffer.hpp/.cpp` | 64-frame snapshot + input ring (unchanged capacity) |
| `RollbackManager` | `rollback.hpp/.cpp` | Records predicted locals; reconciles on remote apply |
| Transport / deltas | `transport.hpp`, `snapshot_delta.hpp` | Loopback + delta stubs from prior deepen PR |

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

Clients predict with local input before authoritative packets arrive. `InputHistoryBuffer` stores both sides per frame:

```cpp
#include <fuse/net/input_history.hpp>

fuse::net::InputHistoryBuffer history;
history.init(128);

fuse::net::PlayerInput local{};
local.frame = frame;
local.axis_lx = stick_x;
history.store_predicted(frame, local);

// later, when authoritative input arrives:
fuse::net::ReconcileResult result =
    fuse::net::reconcile_predicted_input(history, frame, authoritative);
// result.action == Confirmed | Mismatch | NoOp
```

`RollbackManager::set_local_input` mirrors predictions into the attached history buffer. `apply_remote_input` records confirmed remotes via the same reconcile helper before resimulation.

---

## Checksum helpers

Snapshot checksums use shared FNV-1a helpers so rollback capture and delta verification stay aligned:

```cpp
#include <fuse/net/checksum.hpp>

snapshot.checksum = fuse::net::compute_snapshot_checksum(snapshot);
const bool ok = fuse::net::verify_snapshot_checksum(snapshot);
```

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
| `test_net_input_history` | Predicted/confirmed retention, ring eviction, `inputs_equal` |
| `test_net_reconcile` | Confirmed / mismatch / NoOp reconcile paths |
| `fuse_net_b74` (umbrella) | Transport, serializer, rollback, buffer, delta, interpolation, AOI |

---

## Gates (B7.4 deepen)

- [x] Interest management AOI stubs (`InterestManager`, relevance radii, priority queue)
- [x] Expanded input history ring (128 frames, predicted + confirmed)
- [x] Reconcile stub (`reconcile_predicted_input`)
- [x] Shared checksum helpers + tests
- [x] `RollbackManager` records predictions and reconciles remotes
- [ ] ENet / Steam real backends (follow-up)
- [ ] Process-pair net smoke (follow-up, see [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md))

---

## Related docs

- [Source/FUSE/Net/README.md](../../Source/FUSE/Net/README.md) — module layout
- [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md) — B7.1–B7.10 integration checklist
