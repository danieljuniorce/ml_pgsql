#include "CPostgresConnection.h"
#include <vector>

/* Collect Lua string args from startIdx to top of stack (correct order for $1..$N). */
static std::vector<const char*> collectArgs(lua_State* luaVM, int startIdx)
{
    int count = lua_gettop(luaVM);
    std::vector<const char*> args;
    args.reserve(static_cast<size_t>(count - startIdx + 1));
    for (int i = startIdx; i <= count; i++)
        args.push_back(luaL_checkstring(luaVM, i));
    return args;
}

CPostgresConnection::CPostgresConnection(lua_State* pLuaVM, const char* szConnectionInfo)
    : m_pLuaVM{pLuaVM}
{
    m_pConnection = PQconnectdb(szConnectionInfo);
    if (IsConnected())
    {
        if (PQsetnonblocking(m_pConnection, 1) == -1)
        {
            PQfinish(m_pConnection);
            m_pConnection = nullptr;
        }
    }
}

CPostgresConnection::~CPostgresConnection()
{
    PQfinish(m_pConnection);
}

/* Stack on entry: sql [, param1, param2, ...] */
bool CPostgresConnection::SendQuery(lua_State* luaVM)
{
    const char* query = luaL_checkstring(luaVM, 1);
    auto args = collectArgs(luaVM, 2);
    int ok = PQsendQueryParams(m_pConnection, query,
                               static_cast<int>(args.size()), NULL,
                               args.data(), NULL, NULL, 0);
    if (ok == 1) { m_bQueryInFlight = true; return true; }
    return false;
}

/* Identical send path — distinction is in result status check inside ProcessPendingQueries. */
bool CPostgresConnection::SendExec(lua_State* luaVM)
{
    const char* query = luaL_checkstring(luaVM, 1);
    auto args = collectArgs(luaVM, 2);
    int ok = PQsendQueryParams(m_pConnection, query,
                               static_cast<int>(args.size()), NULL,
                               args.data(), NULL, NULL, 0);
    if (ok == 1) { m_bQueryInFlight = true; return true; }
    return false;
}

/* Synchronous prepare (fast — only sends the parse message, no data). */
bool CPostgresConnection::Prepare(const char* stmtName, const char* query)
{
    PGresult* res = PQprepare(m_pConnection, stmtName, query, 0, NULL);
    bool ok = res && PQresultStatus(res) == PGRES_COMMAND_OK;
    if (res) PQclear(res);
    return ok;
}

/* Stack on entry: [param1, param2, ...] */
bool CPostgresConnection::SendQueryPrepared(lua_State* luaVM, const char* stmtName)
{
    auto args = collectArgs(luaVM, 1);
    int ok = PQsendQueryPrepared(m_pConnection, stmtName,
                                 static_cast<int>(args.size()),
                                 args.data(), NULL, NULL, 0);
    if (ok == 1) { m_bQueryInFlight = true; return true; }
    return false;
}
