## Overview

This PR refactors `ml_pgsql` from a blocking synchronous module into a fully asynchronous,
thread-safe PostgreSQL module for MTA:SA, aligning it with the pattern used by the native
MySQL module (`dbQuery` / `dbExec` callbacks).

The original module had three critical bugs that made it functionally broken in several
common scenarios, plus a blocking design that could freeze the MTA server on any slow query.

---

## Breaking Changes

`pg_query` and `pg_exec` now require a **callback** as the second argument.

| | Before | After |
|---|---|---|
| **Signature** | `pg_query(conn, sql, ...)` | `pg_query(conn, callback, sql, ...)` |
| **Return** | `PGresult*` userdata (blocking) | `true` (async, callback fires later) |

---

## Bug Fixes

| # | File | Bug | Fix |
|---|------|-----|-----|
| 1 | `CPostgresConnection.cpp` | `Exec()` checked `PGRES_TUPLES_OK` (SELECT status) on INSERT/UPDATE/DELETE — always returned `false` | Changed to `PGRES_COMMAND_OK` |
| 2 | `CPostgresConnection.cpp` | Param collection loop iterated in reverse → `$1` received the last argument, `$N` received the first | Changed to forward iteration `for (i=1; i<=n; ++i)` |
| 3 | `CFunctions.cpp` (`pg_poll`) | Row tables were built correctly but never inserted into the outer table — `pg_poll` always returned an empty table `{}` | Added `lua_rawseti(luaVM, -2, i+1)` after each row |

---

## New Architecture (Async)

```
pg_query(conn, callback, sql, params...)
  │
  ├─ PQsendQueryParams()  ← non-blocking send, returns immediately
  └─ stores {conn, callbackRef, isExec=false} in pending map

DoPulse() — called every MTA server tick (~60 Hz)
  └─ ProcessPendingQueries()
       ├─ PQconsumeInput()     ← receive network data
       ├─ PQisBusy() == false  ← result ready?
       ├─ PQgetResult()        ← collect result
       ├─ lua_rawgeti + lua_pcall  ← fire Lua callback
       ├─ drain remaining PQgetResult() (required by libpq protocol)
       └─ luaL_unref()         ← release Lua registry reference
```

Key changes in `CPostgresConnection`:
- Constructor calls `PQsetnonblocking(conn, 1)` after connecting
- `Query()` / `Exec()` replaced by `SendQuery()` / `SendExec()` using `PQsendQueryParams()`

---

## Performance Improvements

| Area | Before | After | Impact |
|------|--------|-------|--------|
| Connection lookup | `std::vector` O(n) | `std::unordered_set` O(1) | Scales with connection count |
| Pending query lookup | `std::vector` O(n) | `std::unordered_map<conn*, query>` O(1) | Scales with concurrent queries |
| Lua result table | `lua_newtable()` no-hint | `lua_createtable(nrows, ncols)` pre-sized | Eliminates rehash for large result sets |
| Param vector | `push_back` without reserve | `args.reserve(count)` upfront | Eliminates reallocation |
| Query string | `std::string` heap copy | `const char*` direct from Lua stack | Removes unnecessary allocation |

---

## Thread Safety

- `std::mutex m_mutex` added to `CPostgresManager`
- `std::lock_guard<std::mutex>` wraps all public methods:
  `Add`, `CloseAllConnections`, `RemoveConnection`, `AddPendingQuery`, `ProcessPendingQueries`
- Callback refs are unref'd before removing connections (no dangling registry entries)

---

## New Features

### `pg_prepare(conn, name, sql)` → `boolean`
Sends a named prepared statement to the PostgreSQL server once at resource startup.
Uses `PQprepare()` synchronously (fast — only a parse message, no data transfer).

### `pg_query_prepared(conn, name, callback, ...)` → `true | false, errmsg`
Executes a previously prepared statement via `PQsendQueryPrepared()`.
PostgreSQL skips parse and plan on every call — significant latency reduction for
frequently repeated queries (e.g., player data lookups).

---

## New Lua API

```lua
local conn = pg_conn("postgresql://user:pass@host:5432/db")

-- Async SELECT
pg_query(conn, function(result)
    if result then
        local rows = pg_poll(result)   -- pg_poll signature unchanged
        for _, row in ipairs(rows) do
            print(row.id, row.nome)
        end
    end
end, "SELECT id, nome FROM players WHERE nome = $1", playerName)

-- Async INSERT / UPDATE / DELETE
pg_exec(conn, function(ok)
    if ok then print("saved") end
end, "INSERT INTO players (nome) VALUES ($1)", playerName)

-- Prepared statement (call pg_prepare once at startup)
pg_prepare(conn, "get_player", "SELECT * FROM players WHERE id = $1")

pg_query_prepared(conn, "get_player", function(result)
    if result then
        local rows = pg_poll(result)
    end
end, playerId)

pg_close(conn)
```

---

## CI/CD

Added `.github/workflows/build.yml`:
- **On every push / PR**: builds Windows x86, Windows x64, Linux x64
- **On `v*` tag push**: publishes a GitHub Release with three artifacts:
  `ml_pgsql32.dll`, `ml_pgsql64.dll`, `ml_pgsql64.so`

---

## Tested On
- Windows x64, Visual Studio 2022, MTA:SA 1.6 server
- PostgreSQL 15
