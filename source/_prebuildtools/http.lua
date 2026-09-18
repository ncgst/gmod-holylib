--[[
	tbl = {
		failed = function(reason) end,
		success = function(body) end,
		method = "GET POST HEAD PUT DELETE PATCH OPTIONS",
		url = "www.google.com",
		body = "",
		type = "text/plain; charset=utf-8",
		timeout = 60,
		headers = {
			["APIKey"] = "SaliBonani" 
		}
	}

	ToDo: Make this far better
]]

RemoveDir("http") -- Nuke old data!
CreateDir("http")

local requests = {}
local requestComplete = "HOLYLIB_HTTP_COMPLETE"

local function RequestFailed(tbl, reason)
	if tbl.failed then
		tbl.failed(reason)
	else
		error(reason)
	end
	return false
end

local function FinishRequest(tbl)
	if not tbl.handle then
		return RequestFailed(tbl, "Request failed: could not start curl")
	end

	-- Curl writes the response to a file and only emits this marker on success.
	-- Waiting for pipe EOF precedes reading that file; a stable size is not EOF.
	-- Lua 5.1's popen close can report true even for a nonzero process exit.
	local completion = tbl.handle:read("*a")
	local closed = tbl.handle:close()
	tbl.handle = nil
	if not closed or not completion or not completion:match("^" .. requestComplete .. "%s*$") then
		return RequestFailed(tbl, "Request failed: curl did not complete successfully")
	end

	local httpcontent = ReadFile(tbl.httpfile)
	if httpcontent == nil then
		return RequestFailed(tbl, "Request failed: response file is missing")
	end
	if tbl.success then tbl.success(httpcontent) end
	return true
end

function HTTP_WaitForAllInternal()
	local tbl = table.remove(requests, 1)
	if not tbl then return false end
	FinishRequest(tbl)
	return #requests > 0
end

function HTTP_WaitForAll()
	while HTTP_WaitForAllInternal() do end
end

local function CopyTable(input, references)
	local output = {}
	references = references or {} -- to prevent loops
	if references[input] then
		print("CopyTable was called with looping references!")
		return output
	end
	references[input] = true

	for key, value in pairs(input) do
		if type(value) == "table" then
			output[key] = CopyTable(value, references)
		else
			output[key] = value
		end
	end

	return output
end

local i = 0
function HTTP(inputTbl)
	i = i + 1

	local tbl = CopyTable(inputTbl) -- we don't want to modify the input table!
	local method = tbl.method or "GET"
	local url = tbl.url or ""
	local body = tbl.body or ""
	local contentType = tbl.type or ""
	local timeout = tbl.timeout or 5
	local headers = ""
	local params = ""
	if tbl.params then
		if type(tbl.params) == "string" then
			params = " --data-urlencode \"" .. tbl.params .. "\""
		else
			for _, param in ipairs(tbl.params) do
				params = params .. " --data-urlencode \"" .. param .. "\""
			end
		end
	end

	if tbl.headers then
		for key, value in pairs(tbl.headers) do
			headers = headers .. " -H \"" .. key .. ":" .. value .. "\""
		end
	end
	tbl.httpfile = "http/" .. i .. ".txt"
	local curlCommand = "curl -sS --fail -X " .. method .. " " .. url .. params .. (not (contentType == "") and (" -H \"Content-Type:".. contentType .. "\"") or "") .. headers .. (body == "" and "" or (" --data-raw \"" .. body .. "\"")) .. " --max-time " .. timeout .. " > " .. tbl.httpfile .. " && echo " .. requestComplete
	local handle = io.popen(curlCommand)
	tbl.handle = handle

	if not tbl.mode or tbl.mode == "async" then
		table.insert(requests, tbl)
	elseif tbl.mode == "sync" then
		FinishRequest(tbl)
	end
end

function HTTPDownload(tbl)
	i = i + 1

	local url = tbl.url or ""
	tbl.httpfile = tbl.file or "http/" .. i .. ".txt"
	local timeout = tbl.timeout or 15
	local headers = ""
	local params = ""
	if tbl.params then
		if type(tbl.params) == "string" then
			params = " --data-urlencode \"" .. tbl.params .. "\""
		else
			for _, param in ipairs(tbl.params) do
				params = params .. " --data-urlencode \"" .. param .. "\""
			end
		end
	end

	if tbl.headers then
		for key, value in pairs(tbl.headers) do
			headers = headers .. " -H \"" .. key .. ":" .. value .. "\""
		end
	end

	local curlCommand = "curl -L " .. url .. params .. headers .. " --max-time " .. timeout .. " -sS --fail -o \"" .. tbl.httpfile .. "\" && echo " .. requestComplete
	local handle = io.popen(curlCommand)
	tbl.handle = handle

	table.insert(requests, tbl)
end

function JSONHTTP(tbl)
	local func = tbl.success
	tbl.success = function(body)
		local json = json.decode(body)
		if func then
			func(json)
		end
	end
	tbl.headers = tbl.headers or {}
	tbl.headers["Accept"] = "application/json"

	HTTP(tbl)
end
