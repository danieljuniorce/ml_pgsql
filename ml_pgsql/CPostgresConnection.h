#pragma once

#include "ml_pgsql.h"

class CPostgresConnection
{
private:
    lua_State* m_pLuaVM      = nullptr;
    PGconn*    m_pConnection = nullptr;
    bool       m_bQueryInFlight = false;

public:
    CPostgresConnection(lua_State* pLuaVM, const char* szConnectionInfo);
    ~CPostgresConnection();

    lua_State*  GetVM()               { return m_pLuaVM; }
    PGconn*     GetConnection()       { return m_pConnection; }
    bool        IsConnected()         { return PQstatus(m_pConnection) == CONNECTION_OK; }
    const char* GetLastErrorMessage() { return PQerrorMessage(m_pConnection); }

    bool IsQueryInFlight() const  { return m_bQueryInFlight; }
    void SetQueryInFlight(bool v) { m_bQueryInFlight = v; }

    /* Non-blocking send — stack entry point: sql [, param1, ...] */
    bool SendQuery(lua_State* luaVM);
    bool SendExec(lua_State* luaVM);

    /* Prepared statement support */
    bool Prepare(const char* stmtName, const char* query);
    bool SendQueryPrepared(lua_State* luaVM, const char* stmtName);
};
