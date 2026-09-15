# UAISK template hooks (scripts only)

The Universal AI Starter Kit has **no C++ engine patches**. FUSE maps its TorqueScript modules to `fuse_ai` registry type ids for future script-host import.

| UAISK script (ore) | FUSE registry id | Notes |
|--------------------|------------------|-------|
| `aiBehaviors.cs` | `bb.selector` | Behavior objects (`LeashedBehavior`, …) → composite selector trees |
| `aiMovement.cs` | `gb.action.move_toward` | Patrol/chase helpers → GuideBot-inspired move leaf |
| `aiActions.cs` | `bb.action.set_flag` | Action executes → blackboard commit |
| `aiTargeting.cs` | `bb.condition.distance_less` | Range checks → distance condition |

Ore path: `third_party/addons/UAISK/The_Universal_AI_Starter_Kit/Templates/Full/game/scripts/server/UAISK/`

C++ mapping header: `Source/FUSE/Modules/ai/include/fuse/ai/uaisk_template_hooks.hpp`

License: MIT (`third_party/addons/UAISK/LICENSE`).
