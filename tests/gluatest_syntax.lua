-- Parse the test suite without executing engine-dependent code.
-- Paths arrive on stdin so filenames never become shell arguments or Lua code.
local failed = false
local count = 0
for path in io.lines() do
    count = count + 1
    local chunk, err = loadfile(path)
    if not chunk then
        io.stderr:write(err, "\n")
        failed = true
    end
end

if count == 0 then
    io.stderr:write("No GLuaTest files found\n")
    failed = true
end
print("Checked " .. count .. " GLuaTest files")
os.exit(failed and 1 or 0)
