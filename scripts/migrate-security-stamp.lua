-- Stop every server using these namespaces before running this migration.
-- EVAL <script> 0 <session_namespace> <email_namespace>
local sessions = ARGV[1] or 'auth:session'
local emails = ARGV[2] or 'auth:email'
local fields = {}
local keys = {}
local session_count = 0
local code_count = 0

local function scan(prefix, visit)
  local cursor = '0'
  local seen = {}
  repeat
    local page = redis.call('SCAN', cursor, 'COUNT', 500)
    cursor = page[1]
    for _, key in ipairs(page[2]) do
      if string.sub(key, 1, #prefix) == prefix and not seen[key] then
        seen[key] = true
        visit(key, string.sub(key, #prefix + 1))
      end
    end
  until cursor == '0'
end

local function field(key)
  local old = redis.call('HGET', key, 'generation')
  if not old then return false end
  local current = redis.call('HGET', key, 'security_stamp')
  if current and current ~= old then
    error('Conflicting security_stamp; no changes applied')
  end
  table.insert(fields, {key, old})
  return true
end

-- Validate the entire plan before changing any data.
scan(sessions .. ':', function(key, suffix)
  if #suffix == 64 and string.match(suffix, '^[0-9a-f]+$') then
    if field(key) then session_count = session_count + 1 end
  end
end)
scan(emails .. ':', function(key, suffix)
  if string.match(suffix, '^%d+:bind$') or string.match(suffix, '^%d+:reset$') then
    if field(key) then code_count = code_count + 1 end
  elseif string.match(suffix, '^%d+:generation$') then
    local target = string.sub(key, 1, -12) .. ':security_stamp'
    local old = redis.call('GET', key)
    local current = redis.call('GET', target)
    if current and current ~= old then
      error('Conflicting email security_stamp; no changes applied')
    end
    table.insert(keys, {key, target})
  end
end)

for _, item in ipairs(fields) do
  redis.call('HSET', item[1], 'security_stamp', item[2])
  redis.call('HDEL', item[1], 'generation')
end
for _, item in ipairs(keys) do
  redis.call('RENAME', item[1], item[2])
end
return {session_count, code_count, #keys}
