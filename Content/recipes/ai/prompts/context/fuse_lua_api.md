# FUSE Lua behaviour API (context for SUMMARISE_CODE / PORT_SCRIPT, plan §4.5)

Shared context for the Gemini agent (cached with context caching) and the local-llm fallback. It describes
our engine, not any game, so it is safe to send with every request.

## Behaviours

A behaviour is a Lua chunk run by `ScriptRuntime` in a sandbox: no `io`, `package`, `debug`, `require`,
`load`, `loadfile`, `dofile` or `collectgarbage`; `os` keeps only `clock`, `time`, `date` and `difftime`;
`string.dump` is removed. The chunk defines global callbacks, each called with `self` first:

- `on_start(self)`, `on_update(self, dt)`, `on_destroy(self)`
- `on_collision(self, other, point)` - first physics step of a contact; `point` is `{x, y, z}`
- `on_trigger_enter(self, other)`, `on_trigger_exit(self, other)`

`self` is the behaviour instance table. Properties authored on the entity (legacy datablock fields are
mapped to snake_case properties) are fields of `self`.

## Bindings

- `Entity.create()`, `Entity.destroy(e)`, `Entity.alive(e)`
- `Entity.get_position(e)` / `Entity.set_position(e, {x, y, z})`, `Entity.get_rotation(e)`,
  `Entity.set_rotation_euler(e, {x, y, z})`
- `Physics.ray_cast(...)`, `Physics.apply_impulse(e, v)`, `Physics.set_velocity(e, v)`, `Physics.get_velocity(e)`
- `Scene.find_entity(name)`, `Scene.query_sphere(centre, radius)`
- `Audio.load(path)`, `Audio.play_at(sound, pos)`, `Audio.play_2d(sound)`
- `Input.*`, `CVar.get(name)` / `CVar.set(name, value)`

## Port bridge (Remaster)

Gameplay state that has no engine component yet goes through the port bridge, which the parity harness
records as the event log:

- `Remaster.get(e, field)` / `Remaster.set(e, field, value)` - per-entity gameplay fields (for example
  `damage_state`: `Enabled`, `Disabled`, `Destroyed`)
- `Remaster.emit(name, fields)` - a gameplay event (`damage`, `pickup`, `door`, `score`...); entity fields
  are named `target`, `source`, `entity` or `other`

## Rules for ports

- Reimplement from the spec. Never paste original script text into code or comments.
- Keep each legacy handler's guards and effects; name the callbacks after the spec's events.
- Only the APIs above. No globals other than the callbacks; helpers are `local`.
