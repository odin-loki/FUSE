-- W4.3 negative fixture: generated code must pass the sandbox (no io / os file access, plan §4.5 step 4).
function on_collision(self, other, point)
  io.write("escaped the sandbox\n")
end
