#include "CPostgresManager.h"

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
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto it = m_pendingQueries.begin(); it != m_pendingQueries.end(); )
    {
        CPostgresConnection* pConn = it->first;
        PendingQuery&        pq    = it->second;
        PGconn*              conn  = pConn->GetConnection();

        PQconsumeInput(conn);
        if (PQisBusy(conn)) { ++it; continue; }

        PGresult* result = PQgetResult(conn);

        lua_rawgeti(pq.luaVM, LUA_REGISTRYINDEX, pq.callbackRef);

        if (pq.isExec)
        {
            bool ok = result && PQresultStatus(result) == PGRES_COMMAND_OK;
            if (result) PQclear(result);
            lua_pushboolean(pq.luaVM, ok);
            lua_pcall(pq.luaVM, 1, 0, 0);
        }
        else
        {
            if (result && PQresultStatus(result) == PGRES_TUPLES_OK)
                lua_pushlightuserdata(pq.luaVM, result);
            else
            {
                if (result) PQclear(result);
                lua_pushboolean(pq.luaVM, false);
            }
            lua_pcall(pq.luaVM, 1, 0, 0);
        }

        /* Drain remaining results — required by libpq non-blocking protocol. */
        PGresult* extra;
        while ((extra = PQgetResult(conn)) != nullptr) PQclear(extra);

        luaL_unref(pq.luaVM, LUA_REGISTRYINDEX, pq.callbackRef);
        pConn->SetQueryInFlight(false);
        it = m_pendingQueries.erase(it);
    }
}
