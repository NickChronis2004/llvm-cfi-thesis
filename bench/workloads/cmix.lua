-- calls 8 different C functions in pseudo-random order
local fns = { math.abs, math.floor, math.ceil, math.sqrt,
              math.max, math.min, math.sin, math.cos }
local x, s = 12345, 0
for i = 1, 20000000 do
  x = (x * 1103515245 + 12345) & 0x7fffffff   -- LCG, same cost for every variant
  local f = fns[((x >> 16) % 8) + 1]
  s = s + f((x & 1023) / 7)
end
print(s)
