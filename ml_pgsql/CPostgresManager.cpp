#include "CPostgresManager.h"
#include <vector>
#include <cstdio>

#ifndef LUA_OK
#define LUA_OK 0
#endif

void CPostgresManager::Add(CPostgresConnection* pConn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_setConnections.insert(pConn);
}

CPostgresConnection* CPostgresManager::NewConnection(lua_State* pLuaVM)
{
    const char* szConnectionInfo = luaL_checkstring(pLuaVM, 1);
    CPostgresConnection* pConnection = new CPostgresConnection(pLuaVM, szConnectionInfo);
    if (pConnection && pConnection->IsConnected())
        g_pPostgresManager->Add(pConnection);
    return pConnection;
}

bool CPostgresManager::IsLive(CPostgresConnection* pConn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_setConnections.count(pConn) != 0;
}

void CPostgresManager::CloseAllConnections(lua_State* pLuaVM)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_setConnections.begin(); it != m_setConnections.end(); )
    {
        CPostgresConnection* pConn = *it;
        if (!pLuaVM || pConn->GetVM() == pLuaVM)
        {
            auto pq = m_pendingQueries.find(pConn);
            if (pq != m_pendingQueries.end())
            {
                luaL_unref(pq->second.luaVM, LUA_REGISTRYINDEX, pq->second.callbackRef);
                m_pendingQueries.erase(pq);
            }
            SAFE_DELETE(pConn);
            it = m_setConnections.erase(it);
        }
        else
            ++it;
    }
}

void CPostgresManager::RemoveConnection(CPostgresConnection* pConnection)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto pq = m_pendingQueries.find(pConnection);
    if (pq != m_pendingQueries.end())
    {
        luaL_unref(pq->second.luaVM, LUA_REGISTRYINDEX, pq->second.callbackRef);
        m_pendingQueries.erase(pq);
    }
    m_setConnections.erase(pConnection);
    SAFE_DELETE(pConnection);
}

void CPostgresManager::AddPendingQuery(CPostgresConnection* pConn, PendingQuery q)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingQueries[pConn] = q;
}

void CPostgresManager::ProcessPendingQueries()
{
    struct ReadyEntry {
        CPostgresConnection* pConn;
        PendingQuery         pq;
        PGresult*            result;
    };
    std::vector<ReadyEntry> ready;

    /* Collect ready entries under the lock, then release before invoking Lua callbacks.
     * Holding m_mutex during lua_pcall would deadlock if any callback calls pg_query/pg_exec. */
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pendingQueries.begin(); it != m_pendingQueries.end(); )
        {
            CPostgresConnection* pConn = it->first;
            PGconn*              conn  = pConn->GetConnection();

            PQconsumeInput(conn);
            if (PQisBusy(conn)) { ++it; continue; }

            PGresult* result = PQgetResult(conn);
            ready.push_back({ pConn, it->second, result });
            pConn->SetQueryInFlight(false);
            it = m_pendingQueries.erase(it);
        }
    }

    for (auto& e : ready)
    {
        PendingQuery& pq   = e.pq;
        PGconn*       conn = e.pConn->GetConnection();

        lua_rawgeti(pq.luaVM, LUA_REGISTRYINDEX, pq.callbackRef);

        if (pq.isExec)
        {
            bool ok = e.result && PQresultStatus(e.result) == PGRES_COMMAND_OK;
            if (e.result) PQclear(e.result);
            lua_pushboolean(pq.luaVM, ok);
        }
        else
        {
            if (e.result && PQresultStatus(e.result) == PGRES_TUPLES_OK)
                lua_pushlightuserdata(pq.luaVM, e.result);
            else
            {
                if (e.result) PQclear(e.result);
                lua_pushboolean(pq.luaVM, false);
            }
        }

        int rc = lua_pcall(pq.luaVM, 1, 0, 0);
        if (rc != LUA_OK)
        {
            const char* err = lua_tostring(pq.luaVM, -1);
            if (err)
                printf("[ml_pgsql] Lua callback error: %s\n", err);
            lua_pop(pq.luaVM, 1);
        }

        /* Drain remaining results — required by libpq non-blocking protocol. */
        PGresult* extra;
        while ((extra = PQgetResult(conn)) != nullptr) PQclear(extra);

        luaL_unref(pq.luaVM, LUA_REGISTRYINDEX, pq.callbackRef);
    }
}
