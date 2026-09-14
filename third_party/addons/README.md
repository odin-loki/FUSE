# Torque community addons (reference / compat research)

These trees are **git submodules** of the TorqueGameEngines snapshots. They are **not** production FUSE dependencies — use them while porting gameplay/AI/FX ideas into FUSE Track B / compat.

| Directory | Upstream | Role |
|-----------|----------|------|
| `GMK/` | TorqueGameEngines/Addon-GMK | Game Mechanics Kit |
| `Verve/` | TorqueGameEngines/Addon-Verve | Cinematics / cutscenes |
| `BadBehaviour/` | TorqueGameEngines/Addon-BadBehaviour | Behavior trees |
| `GuideBot/` | TorqueGameEngines/Addon-GuideBot | AI |
| `UAISK/` | TorqueGameEngines/UAISK | Universal AI Starter Kit |
| `AFX-Template/` | TorqueGameEngines/AFX-Template | AFX spell/FX template |
| `3DAAK/` | TorqueGameEngines/Addon-3DAAK | 3D Action Adventure Kit |

Clone with: `git clone --recurse-submodules https://github.com/odin-loki/FUSE.git`  
Or after clone: `git submodule update --init --recursive`
