-- shapebase_impact: FUSE behaviour reimplemented from spec shapebase_impact (FUSE Remaster W4.3 fixture).
-- Written from the spec, not from the original script text (plan §4.5 step 4).
-- Properties: min_impact_speed, collision_mul, impact_mul (from the datablock).

local function apply_impact(self, other, vec, len, multiplier)
  if not Entity.alive(self) or Remaster.get(self, "damage_state") == "Destroyed" then
    return
  end
  local p = Entity.get_position(self)
  local at = {x = p.x - vec.x, y = p.y - vec.y, z = p.z - vec.z}
  local amount = len / self.min_impact_speed * multiplier
  Remaster.emit("damage", {target = self, source = other, position = at, amount = amount, kind = "impact"})
end

-- The engine's collision callback carries no velocity: vec is this entity's velocity at contact.
function on_collision(self, other, point)
  local v = Physics.get_velocity(self)
  local len = math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
  apply_impact(self, other, v, len, self.collision_mul)
end

function on_impact(self, other, vec, len)
  apply_impact(self, other, vec, len, self.impact_mul)
end
