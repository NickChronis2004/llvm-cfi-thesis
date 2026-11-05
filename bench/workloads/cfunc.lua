local abs = math.abs
local s = 0
for i = 1, 20000000 do s = s + abs(-i) end
print(s)
