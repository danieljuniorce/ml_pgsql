#include "ml_pgsql.h"
#include "CFunctions.h"
#include "CPostgresManager.h"

/*
 * Helper: push callback from arg2 into registry, remove conn (1) and callback (2)
 * from the Lua stack so that sql becomes arg 1 and params start at arg 2.
 * Returns the registry reference for the callback.
 */
static int storeCallbackRef(lua_State* luaVM)
{
    lua_pushvalue(luaVM, 2);
    int ref = luaL_ref(luaVM, LUA_REGISTRYINDEX);
    lua_remove(luaVM, 1);  // conn
    lua_remove(luaVM, 1);  // callback  →  sql is now at index 1
    return ref;
}

/* pg_conn(connstr) → conn | false, errmsg */
int CFunctions::pg_conn(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_conn", lua_gettop(luaVM) == 1);

    CPostgresConnection* pConn = CPostgresManager::NewConnection(luaVM);
    if (pConn && pConn->IsConnected())
    {
        lua_pushlightuserdata(luaVM, pConn);
        return 1;
    }
    else if (pConn)
    {
        lua_pushboolean(luaVM, false);
        lua_pushstring(luaVM, pConn->GetLastErrorMessage());
        SAFE_DELETE(pConn);
        return 2;
    }
    lua_pushboolean(luaVM, false);
    return 1;
}

/* pg_query(conn, callback, sql [, param1, ...]) → true | false, errmsg
 * callback(result)  — result is PGresult* userdata; pass to pg_poll(), or false on error. */
int CFunctions::pg_query(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_query", lua_gettop(luaVM) >= 3);
    LUA_FUNCTION_ASSERT("pg_query", lua_isfunction(luaVM, 2));

    auto* pConn = static_cast<CPostgresConnection*>(lua_touserdata(luaVM, 1));
    LUA_FUNCTION_ASSERT("pg_query", pConn && pConn->IsConnected());
    LUA_FUNCTION_ASSERT("pg_query", !pConn->IsQueryInFlight());

    int ref = storeCallbackRef(luaVM);

    if (!pConn->SendQuery(luaVM))
    {
        luaL_unref(luaVM, LUA_REGISTRYINDEX, ref);
        lua_pushboolean(luaVM, false);
        lua_pushstring(luaVM, PQerrorMessage(pConn->GetConnection()));
        return 2;
    }

    g_pPostgresManager->AddPendingQuery(pConn, { luaVM, ref, false });
    lua_pushboolean(luaVM, true);
    return 1;
}

/* pg_exec(conn, callback, sql [, param1, ...]) → true | false, errmsg
 * callback(ok)  — ok is boolean. */
int CFunctions::pg_exec(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_exec", lua_gettop(luaVM) >= 3);
    LUA_FUNCTION_ASSERT("pg_exec", lua_isfunction(luaVM, 2));

    auto* pConn = static_cast<CPostgresConnection*>(lua_touserdata(luaVM, 1));
    LUA_FUNCTION_ASSERT("pg_exec", pConn && pConn->IsConnected());
    LUA_FUNCTION_ASSERT("pg_exec", !pConn->IsQueryInFlight());

    int ref = storeCallbackRef(luaVM);

    if (!pConn->SendExec(luaVM))
    {
        luaL_unref(luaVM, LUA_REGISTRYINDEX, ref);
        lua_pushboolean(luaVM, false);
        lua_pushstring(luaVM, PQerrorMessage(pConn->GetConnection()));
        return 2;
    }

    g_pPostgresManager->AddPendingQuery(pConn, { luaVM, ref, true });
    lua_pushboolean(luaVM, true);
    return 1;
}

/* pg_poll(result) → table of rows  (call inside pg_query callback)
 * Each row is a table keyed by column name.
 * Frees the PGresult* automatically. */
int CFunctions::pg_poll(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_poll", lua_gettop(luaVM) == 1);

    PGresult* pResult = static_cast<PGresult*>(lua_touserdata(luaVM, 1));
    if (pResult)
    {
        int ncols = PQnfields(pResult);
        int nrows = PQntuples(pResult);

        lua_createtable(luaVM, nrows, 0);  // pre-size array part
        for (int i = 0; i < nrows; i++)
        {
            lua_createtable(luaVM, 0, ncols);  // pre-size hash part
            for (int col = 0; col < ncols; col++)
            {
                lua_pushstring(luaVM, PQfname(pResult, col));
                lua_pushstring(luaVM, PQgetvalue(pResult, i, col));
                lua_settable(luaVM, -3);
            }
            lua_rawseti(luaVM, -2, i + 1);  // outer[i+1] = row
        }

        PQclear(pResult);
        return 1;
    }

    lua_pushboolean(luaVM, false);
    return 1;
}

/* pg_free(result) — discard a PGresult* without polling it. */
int CFunctions::pg_free(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_free", lua_gettop(luaVM) == 1);

    PGresult* pResult = static_cast<PGresult*>(lua_touserdata(luaVM, 1));
    if (pResult)
    {
        PQclear(pResult);
        lua_pushboolean(luaVM, true);
        return 1;
    }

    lua_pushboolean(luaVM, false);
    return 1;
}

/* pg_close(conn) — close connection and free resources. */
int CFunctions::pg_close(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_close", lua_gettop(luaVM) == 1);

    CPostgresConnection* pConn = static_cast<CPostgresConnection*>(lua_touserdata(luaVM, 1));
    if (pConn)
    {
        g_pPostgresManager->RemoveConnection(pConn);
        lua_pushboolean(luaVM, true);
        return 1;
    }

    lua_pushboolean(luaVM, false);
    return 1;
}

/* pg_prepare(conn, name, sql) → boolean
 * Prepares a named statement server-side. Call once at resource start. */
int CFunctions::pg_prepare(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_prepare", lua_gettop(luaVM) == 3);

    auto* pConn = static_cast<CPostgresConnection*>(lua_touserdata(luaVM, 1));
    LUA_FUNCTION_ASSERT("pg_prepare", pConn && pConn->IsConnected());
    LUA_FUNCTION_ASSERT("pg_prepare", !pConn->IsQueryInFlight());

    const char* stmtName = luaL_checkstring(luaVM, 2);
    const char* query    = luaL_checkstring(luaVM, 3);

    lua_pushboolean(luaVM, pConn->Prepare(stmtName, query));
    return 1;
}

/* pg_query_prepared(conn, name, callback [, param1, ...]) → true | false, errmsg
 * Executes a previously prepared statement. callback(result) like pg_query. */
int CFunctions::pg_query_prepared(lua_State* luaVM)
{
    LUA_FUNCTION_ASSERT("pg_query_prepared", lua_gettop(luaVM) >= 3);
    LUA_FUNCTION_ASSERT("pg_query_prepared", lua_isfunction(luaVM, 3));

    auto* pConn = static_cast<CPostgresConnection*>(lua_touserdata(luaVM, 1));
    LUA_FUNCTION_ASSERT("pg_query_prepared", pConn && pConn->IsConnected());
    LUA_FUNCTION_ASSERT("pg_query_prepared", !pConn->IsQueryInFlight());

    const char* stmtName = luaL_checkstring(luaVM, 2);

    lua_pushvalue(luaVM, 3);
    int ref = luaL_ref(luaVM, LUA_REGISTRYINDEX);

    lua_remove(luaVM, 1);  // conn
    lua_remove(luaVM, 1);  // name
    lua_remove(luaVM, 1);  // callback  →  params start at index 1

    if (!pConn->SendQueryPrepared(luaVM, stmtName))
    {
        luaL_unref(luaVM, LUA_REGISTRYINDEX, ref);
        lua_pushboolean(luaVM, false);
        lua_pushstring(luaVM, PQerrorMessage(pConn->GetConnection()));
        return 2;
    }

    g_pPostgresManager->AddPendingQuery(pConn, { luaVM, ref, false });
    lua_pushboolean(luaVM, true);
    return 1;
}
