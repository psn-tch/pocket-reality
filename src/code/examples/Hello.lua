-- Your first program. Press Ctrl+R to run it, and Esc to stop.
print("Hello from Pocket Code!")
print("Press a key to change the color.")

local colors = { RED, GREEN, BLUE, YELLOW, PURPLE }
local n = 1

while true do
    fill(200, 60, 240, 120, colors[n])
    waitkey()
    n = n % #colors + 1
end
