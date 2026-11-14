-- W4.3 negative fixture: a plausible but wrong port (on_impact uses the collision multiplier). The parity
-- harness must report a different event log.
local function apply_impact(self, other, vec, len, multiplier)
  if not Entity.alive(self) or Remaster.get(self, "damage_state") == "Destroyed" then
    return
  end
  local p = Entity.get_position(self)
  local at = {x = p.x - vec.x, y = p.y - vec.y, z = p.z - vec.z}
  Remaster.emit("damage", {target = self, source = other, position = at,
                           amount = len / self.min_impact_speed * multiplier, kind = "impact"})
end

function on_collision(self, other, point)
  local v = Physics.get_velocity(self)
  apply_impact(self, other, v, math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z), self.collision_mul)
end

function on_impact(self, other, vec, len)
  apply_impact(self, other, vec, len, self.collision_mul)
end
