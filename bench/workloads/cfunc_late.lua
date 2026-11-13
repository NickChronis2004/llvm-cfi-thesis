local len = utf8.len
local s = 0
for i = 1, 20000000 do s = s + len("a") end
print(s)
