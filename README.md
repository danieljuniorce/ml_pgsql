# ml_pgsql — PostgreSQL module for MTA:SA

Non-blocking PostgreSQL client module for [Multi Theft Auto: San Andreas](https://multitheftauto.com/) servers.
Exposes 8 Lua functions backed by **libpq** with a callback-driven async model driven by the MTA `DoPulse` heartbeat.

PRs and issues are welcome.

---

## Table of contents

- [How it works](#how-it-works)
- [CI / CD pipeline](#ci--cd-pipeline)
- [Installation](#installation)
- [API reference](#api-reference)
  - [pg\_conn](#pg_conn)
  - [pg\_close](#pg_close)
  - [pg\_query](#pg_query)
  - [pg\_exec](#pg_exec)
  - [pg\_poll](#pg_poll)
  - [pg\_free](#pg_free)
  - [pg\_prepare](#pg_prepare)
  - [pg\_query\_prepared](#pg_query_prepared)
- [Full example](#full-example)

---

## How it works

```
Lua script                 CFunctions           CPostgresConnection      PostgreSQL server
───────────                ──────────           ───────────────────      ─────────────────
pg_query(conn, cb, sql) -> SendQuery()       -> PQsendQueryParams()  --> [network]
                           stores cb ref                                  ...
                                                                          ...result arrives
DoPulse() (every tick)  -> ProcessPendingQueries()
                           PQconsumeInput()
                           PQisBusy() == false
                           PQgetResult()
                           lua_pcall(cb, result) <- PGresult* userdata
inside callback:
  pg_poll(result)        -> PQgetvalue/PQclear() -> Lua table of rows
```

**Key design points:**

- All queries are **fire-and-forget**: `pg_query`/`pg_exec` return immediately with `true` and later invoke the Lua callback.
- One in-flight query per connection at a time. Attempting a second call while one is running returns `false, "query in flight"`.
- `CPostgresManager` holds a `std::mutex` around the pending-query map; callbacks are invoked **outside** the lock to prevent deadlocks if the callback itself fires another query.
- `pg_poll` calls `PQclear()` automatically. Use `pg_free` only when you want to discard a result without reading it.

---

## CI / CD pipeline

### Workflow file

`.github/workflows/build.yml` — single workflow with three jobs:

```
build-windows (x86 + x64, matrix)
build-linux   (x64)
release        (depends on both build jobs)
```

### Triggers

| Event | What happens |
|---|---|
| Push to any branch | Builds run; `release` job is **skipped** |
| Push of a `v*` tag | Builds run; `release` job publishes to GitHub Releases |
| `workflow_dispatch` (manual) | Builds run; `release` job publishes using the supplied version string |

### How to publish a release manually (from any branch)

1. Go to **Actions → Build and Release → Run workflow**.
2. Fill in:
   - **Versão a publicar** — version tag to create (e.g. `v1.3.0`). This becomes the GitHub Release tag.
   - **Marcar como pré-lançamento** — check if this is a pre-release.
3. Click **Run workflow**.

The `release` job resolves the tag at runtime:

```yaml
- name: Resolve release tag
  id: tag
  run: |
    if [ "${{ github.event_name }}" = "workflow_dispatch" ]; then
      echo "name=${{ inputs.version }}" >> "$GITHUB_OUTPUT"
    else
      echo "name=${{ github.ref_name }}" >> "$GITHUB_OUTPUT"
    fi
```

### Build artifacts

Each build job produces an artifact uploaded with `actions/upload-artifact@v4`:

| Artifact name | File inside | Platform |
|---|---|---|
| `ml_pgsql-windows-x86` | `ml_pgsql.dll` | Windows 32-bit |
| `ml_pgsql-windows-x64` | `ml_pgsql.dll` | Windows 64-bit |
| `ml_pgsql-linux-x64` | `ml_pgsql.so` | Linux 64-bit |

> The artifacts are always named `ml_pgsql.dll` / `ml_pgsql.so` (the raw build output).
> The `release` job downloads them and **renames** before attaching to the GitHub Release:

```
ml_pgsql-windows-x86/ml_pgsql.dll  →  ml_pgsql32.dll
ml_pgsql-windows-x64/ml_pgsql.dll  →  ml_pgsql64.dll
ml_pgsql-linux-x64/ml_pgsql.so     →  ml_pgsql64.so
```

### Windows build details

- Uses **MSBuild** with `Premake5 → VS2022` project generation.
- `libpq.lib` (x64) is copied from the pre-installed PostgreSQL on the GitHub runner — no external download needed.
- x86 `libpq.lib` is already committed under `libs/x86/`.

### Linux build details

- Installs `libpq-dev` via `apt`.
- Generates makefiles with `Premake5 gmake2`, builds with `make config=release_x64`.

---

## Installation

1. Download the file matching your server from the [Releases](../../releases) page:

   | File | Platform |
   |---|---|
   | `ml_pgsql32.dll` | Windows x86 (32-bit MTA server) |
   | `ml_pgsql64.dll` | Windows x64 (64-bit MTA server) |
   | `ml_pgsql64.so`  | Linux x64 |

2. Place it in `server/mods/deathmatch/modules/`.

3. Register it in `mtaserver.conf`:

   ```xml
   <!-- Windows x64 -->
   <module src="ml_pgsql64.dll"/>

   <!-- Linux x64 -->
   <module src="ml_pgsql64.so"/>
   ```

4. Restart the server.

---

## API reference

All functions live in the global namespace and are available in any server-side Lua resource after the module is loaded.

### pg_conn

```lua
userdata|false, string pg_conn(string connstr)
```

Opens a new PostgreSQL connection. The connection is **non-blocking** (`PQsetnonblocking`).

**Parameters**

| Name | Type | Description |
|---|---|---|
| `connstr` | string | libpq connection string |

Connection string formats:
- `postgresql://USER:PASSWORD@HOST:PORT/DBNAME?connect_timeout=3`
- `hostaddr=IP port=5432 dbname=DBNAME user=USER password=PASSWORD`

**Returns**

- `conn` (lightuserdata) on success.
- `false, errmsg` on failure.

```lua
local conn, err = pg_conn("postgresql://user:pass@127.0.0.1:5432/mydb?connect_timeout=3")
if not conn then
    print("Connection failed: " .. err)
    return
end
```

---

### pg_close

```lua
bool pg_close(userdata conn)
```

Closes the connection and frees all associated memory. Always call this when the resource stops.

**Returns** `true` on success, `false` if the connection handle is invalid.

```lua
pg_close(conn)
```

---

### pg_query

```lua
true|false, string pg_query(userdata conn, function callback, string sql [, ...params])
```

Sends a **non-blocking SELECT** (or any query that returns rows). Returns immediately; the callback is invoked by `DoPulse` when the result arrives.

**Parameters**

| Name | Type | Description |
|---|---|---|
| `conn` | userdata | Connection handle from `pg_conn` |
| `callback` | function | Called with `(result)` when the query completes |
| `sql` | string | SQL with `$1`, `$2`, … placeholders |
| `...params` | string/number | Values for each placeholder |

**Callback signature**

```lua
function callback(result)
    -- result: PGresult* lightuserdata on success, false on error
    if not result then return end
    local rows = pg_poll(result)   -- converts to Lua table and frees result
end
```

**Returns** `true` if the query was dispatched, `false, errmsg` if the connection is invalid or a query is already in-flight.

```lua
pg_query(conn, function(result)
    if not result then return end
    local rows = pg_poll(result)
    for _, row in ipairs(rows) do
        print(row.id, row.name)
    end
end, "SELECT id, name FROM players WHERE id = $1", playerId)
```

---

### pg_exec

```lua
true|false, string pg_exec(userdata conn, function callback, string sql [, ...params])
```

Sends a **non-blocking command** (INSERT, UPDATE, DELETE, etc.) that does not return rows. Identical call signature to `pg_query`.

**Callback signature**

```lua
function callback(ok)
    -- ok: true on PGRES_COMMAND_OK, false on error
end
```

```lua
pg_exec(conn, function(ok)
    if not ok then print("Insert failed") end
end, "INSERT INTO players (name, score) VALUES ($1, $2)", playerName, 0)
```

---

### pg_poll

```lua
table|false pg_poll(userdata result)
```

Converts a `PGresult*` (received inside a `pg_query` callback) into a Lua table of rows. **Calls `PQclear` automatically** — do not call `pg_free` after `pg_poll`.

**Returns**

An array table where each element is a hash table keyed by column name:

```lua
{
    [1] = { id = "1", name = "Alice", score = "100" },
    [2] = { id = "2", name = "Bob",   score = "200" },
}
```

All values are returned as **strings** (libpq does not carry type information).

Returns `false` if the result handle is nil or already freed.

---

### pg_free

```lua
bool pg_free(userdata result)
```

Discards a `PGresult*` without reading it (calls `PQclear`). Use this when you need to cancel handling of a result inside a callback.

**Returns** `true` on success, `false` if the handle is nil.

```lua
pg_query(conn, function(result)
    if not result then return end
    pg_free(result)   -- discard without processing
end, "SELECT ...")
```

---

### pg_prepare

```lua
bool pg_prepare(userdata conn, string name, string sql)
```

Prepares a named server-side statement. This is a **synchronous** (fast) operation — it only sends the parse message to PostgreSQL. Call once when the resource starts.

**Parameters**

| Name | Type | Description |
|---|---|---|
| `conn` | userdata | Connection handle |
| `name` | string | Unique statement name used later in `pg_query_prepared` |
| `sql` | string | SQL with `$1`, `$2`, … placeholders |

**Returns** `true` on success, `false` on error.

```lua
local ok = pg_prepare(conn, "get_player", "SELECT * FROM players WHERE id = $1")
if not ok then print("Prepare failed") end
```

---

### pg_query_prepared

```lua
true|false, string pg_query_prepared(userdata conn, string name, function callback [, ...params])
```

Executes a previously prepared statement. Non-blocking, same async model as `pg_query`.

**Parameters**

| Name | Type | Description |
|---|---|---|
| `conn` | userdata | Connection handle |
| `name` | string | Statement name passed to `pg_prepare` |
| `callback` | function | Called with `(result)` when the query completes |
| `...params` | string/number | Values for each placeholder |

**Callback** receives a `PGresult*` just like `pg_query` — pass it to `pg_poll`.

```lua
pg_query_prepared(conn, "get_player", function(result)
    if not result then return end
    local rows = pg_poll(result)
    if rows[1] then
        print("Found player: " .. rows[1].name)
    end
end, playerId)
```

---

## Full example

```lua
-- resource: my_resource / server.lua

local conn

addEventHandler("onResourceStart", resourceRoot, function()
    conn, err = pg_conn("postgresql://user:pass@127.0.0.1:5432/mydb?connect_timeout=3")
    if not conn then
        outputServerLog("[pgsql] Connection failed: " .. tostring(err))
        return
    end

    -- prepare a statement once at startup
    pg_prepare(conn, "get_player", "SELECT id, name, score FROM players WHERE id = $1")

    outputServerLog("[pgsql] Connected to PostgreSQL")
end)

addEventHandler("onResourceStop", resourceRoot, function()
    if conn then pg_close(conn) end
end)

-- fetch player using prepared statement
function fetchPlayer(playerId, cb)
    pg_query_prepared(conn, "get_player", function(result)
        if not result then cb(nil) return end
        local rows = pg_poll(result)
        cb(rows[1])
    end, playerId)
end

-- insert player (no rows expected)
function createPlayer(name)
    pg_exec(conn, function(ok)
        if not ok then
            outputServerLog("[pgsql] Failed to create player: " .. name)
        end
    end, "INSERT INTO players (name, score) VALUES ($1, $2)", name, 0)
end

-- ad-hoc query
function getTopPlayers()
    pg_query(conn, function(result)
        if not result then return end
        local rows = pg_poll(result)
        for i, row in ipairs(rows) do
            outputServerLog(i .. ". " .. row.name .. " — " .. row.score)
        end
    end, "SELECT name, score FROM players ORDER BY score DESC LIMIT 10")
end
```
