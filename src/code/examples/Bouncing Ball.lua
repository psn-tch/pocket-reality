-- A ball that bounces. Arrow keys change its speed. Esc stops it.
local x, y = 100, 60
local dx, dy = 4, 3
local size = 12

cls(WHITE)
while true do
    fill(x, y, size, size, WHITE)

    x = x + dx
    y = y + dy
    if x < 0 or x + size > WIDTH then dx = -dx end
    if y < 0 or y + size > HEIGHT then dy = -dy end

    fill(x, y, size, size, RED)
    show()

    local k = key()
    if k == "up" then dy = dy + 1 end
    if k == "down" then dy = dy - 1 end
    if k == "right" then dx = dx + 1 end
    if k == "left" then dx = dx - 1 end
end
