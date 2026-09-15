# Track B — AI / Behavior Trees (U5 `fuse_ai`)

**Status:** U5 vertical slice — registry, composites, decorators, leaf stubs, parallel runtime  
**Work package:** [U5-MODULES.md](./U5-MODULES.md) §3  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §7

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `NodeRegistry` / `NodeLoadSpec` | `Modules/ai/include/fuse/ai/node_registry.hpp` | BadBehaviour `DECLARE_CONOBJECT` analogue |
| `BehaviorTree` / `BehaviorNode` | `Modules/ai/include/fuse/ai/behavior_tree.hpp` | Flat evaluator; job-safe `tick()` |
| `Blackboard` / `BlackboardView` | `Modules/ai/include/fuse/ai/blackboard.hpp` | Per-agent flags; `setFlag` / `getFlag` |
| `BehaviorRuntime` | `Modules/ai/include/fuse/ai/behavior_runtime.hpp` | Snapshot → `parallel_for` eval → commit |
| `loadTreeFromText` | `Modules/ai/include/fuse/ai/tree_loader.hpp` | Compact `.bt`-style text loader |
| UAISK hooks | `Modules/ai/include/fuse/ai/uaisk_template_hooks.hpp` | Script-only template mapping |

**Ore sources (read-only submodules):** BadBehaviour `BadBehavior/`, GuideBot `actionMove.h`, UAISK `.cs` templates — see [Modules/ai/README.md](../../Source/FUSE/Modules/ai/README.md).

**Not in scope (deferred):** Qt BT graph editor, Torque `ScriptedBehavior` VM bridge, 3D navmesh leaves, BehaviorTestbed parity mission.

---

## Built-in node ids

| Type id | Kind | Child status rule |
|---------|------|-------------------|
| `bb.sequence` | Composite | Fail fast on first non-success |
| `bb.selector` | Composite | Succeed fast on first success |
| `bb.parallel` | Composite | Running > Failure > Success; both children ticked |
| `bb.inverter` | Decorator | Flip Success ↔ Failure |
| `bb.loop` | Decorator | Repeat child `loopCount` times |
| `bb.succeed_always` | Decorator | Force Success unless child Running |
| `bb.root` | Decorator | Transparent child pass-through |
| `bb.condition.distance_less` | Leaf | Success when distance < threshold |
| `bb.condition.distance_greater` | Leaf | Success when distance > threshold |
| `bb.condition.blackboard_get` | Leaf | Success when `getFlag(agent, flag)` |
| `bb.action.set_flag` | Leaf | Write flag true |
| `bb.action.blackboard_set` | Leaf | Write flag from `value=` / threshold |
| `bb.action.wait` | Leaf | Running for N ticks (`BehaviorEvalContext`) |
| `bb.action.distance` | Leaf | Write within-range bool to flag |
| `gb.action.move_toward` | Leaf | GuideBot stub; sets flag when far |

---

## Parallel composite aggregation

`bb.parallel` ticks **both** children every frame and merges status:

1. If either child is **Running** → return Running (left child preferred for side effects).
2. Else if either child **Failed** → return Failure (failing child preferred).
3. Else both **Succeeded** → return Success (later child's flag write wins).

`BehaviorRuntime::evaluate` uses a separate axis of parallelism: `JobScheduler::parallel_for` over agents, each with its own `waitStartTicks` slice. Game thread commits `wroteFlag` results via `Blackboard::setFlag`.

---

## Verification

```bash
cmake -B build-fuse -G Ninja -DFUSE_UMBRELLA=ON -DFUSE_BUILD_MODULES=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build-fuse --target fuse_ai_tests
./build-fuse/Source/FUSE/Modules/ai/tests/fuse_ai_tests
```

Tests cover registry parity, composite child-status aggregation (sequence/selector/parallel), blackboard set/get leaves, wait + runtime commit, and multi-agent parallel eval.
