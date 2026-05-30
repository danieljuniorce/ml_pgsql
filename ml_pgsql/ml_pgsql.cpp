#include "ml_pgsql.h"
#include "CFunctions.h"
#include "CPostgresManager.h"


ILuaModuleManager10* g_pLuaModuleManager;
std::unique_ptr<CPostgresManager> g_pPostgresManager;


MTAEXPORT bool InitModule(ILuaModuleManager10 *pManager, char *szModuleName, char *szAuthor, float *fVersion)
{
    g_pLuaModuleManager = pManager;
    g_pPostgresManager.reset(new CPostgresManager());

    strncpy(szModuleName, MODULE_NAME, MAX_INFO_LENGTH);
    strncpy(szAuthor,     MODULE_AUTHOR, MAX_INFO_LENGTH);
    (*fVersion) = MODULE_VERSION;

    return ImportLua();
}


MTAEXPORT void RegisterFunctions(lua_State *luaVM)
{
    if (g_pLuaModuleManager && luaVM)
    {
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_conn",           CFunctions::pg_conn);
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_query",          CFunctions::pg_query);
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_exec",           CFunctions::pg_exec);
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_poll",           CFunctions::pg_poll);
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_free",           CFunctions::pg_free);
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_close",          CFunctions::pg_close);
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_prepare",        CFunctions::pg_prepare);
        g_pLuaModuleManager->RegisterFunction(luaVM, "pg_query_prepared", CFunctions::pg_query_prepared);
    }
}


MTAEXPORT bool DoPulse(void)
{
    g_pPostgresManager->ProcessPendingQueries();
    return true;
}


MTAEXPORT void ResourceStopped(lua_State *luaVM)
{
    g_pPostgresManager->CloseAllConnections(luaVM);
}


MTAEXPORT void ShutdownModule()
{
}
