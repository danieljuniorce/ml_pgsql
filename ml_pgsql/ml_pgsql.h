#pragma once

#ifdef __linux__
#include <cstring>
#endif

/* Module basic configuration */
#define MODULE_NAME     "PostgreSQL"
#define MODULE_AUTHOR   "Disi and xLuxy"
#define MODULE_VERSION  0.7f

/* MTA-SA Module SDK */
#include "Common.h"
#include "ILuaModuleManager.h"

/* Function-related defines for easier working with API */
#define LUA_FUNCTION_DECLARE(function)  static int function(lua_State* luaVM)
#define LUA_FUNCTION_ASSERT(function, expression) { if(!(expression)) { lua_pushboolean(luaVM, 0); lua_pushstring(luaVM, "Assertion failed in " function ": " #expression); return 2; } }

#define SAFE_DELETE(ptr) { delete ptr; ptr = nullptr; }

/* LUA imports */
#include "lua/luaimports.h"

/* Standard Library */
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

/* libpq SDK */
#include <libpq/libpq-fe.h>

/* Represents one pending async query waiting for a result in DoPulse() */
struct PendingQuery {
    lua_State* luaVM;
    int        callbackRef;  // luaL_ref handle into LUA_REGISTRYINDEX
    bool       isExec;       // true = pg_exec (expects PGRES_COMMAND_OK), false = pg_query
};
