#pragma once

#include "ml_pgsql.h"
#include "CPostgresConnection.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

class CPostgresManager
{
private:
    std::unordered_set<CPostgresConnection*>               m_setConnections;
    std::unordered_map<CPostgresConnection*, PendingQuery> m_pendingQueries;
    std::mutex                                             m_mutex;

public:
    CPostgresManager()  = default;
    ~CPostgresManager() { CloseAllConnections(); }

    void Add(CPostgresConnection* pConn);

    static CPostgresConnection* NewConnection(lua_State* pLuaVM);
    void CloseAllConnections(lua_State* pLuaVM = nullptr);
    void RemoveConnection(CPostgresConnection* pConnection);

    void AddPendingQuery(CPostgresConnection* pConn, PendingQuery q);
    void ProcessPendingQueries();
};

extern std::unique_ptr<CPostgresManager> g_pPostgresManager;
