#pragma once

// A singleton's script object (device(), alife(), game_graph(), get_console(), get_hud()), made once per Lua state
// and pointer and then returned again. Their def() getters made a new luabind object at every call, a userdata the Lua
// GC finalizes: device() ~1.7 million and alife() ~1 million in a 3-minute flight with GAMMA. Scripts can only tell by
// identity (two calls now give the same object). Registered over the def(): set_cached_singleton<&get>(L, "name")
namespace script_cached_singleton
{
	template <auto Getter>
	int call(lua_State* L)
	{
		auto* p = Getter();
		if (!p)
		{
			lua_pushnil(L);
			return 1;
		}
		static char key; // one per getter: registry[&key] = {pointer, object}
		lua_pushlightuserdata(L, &key);
		lua_rawget(L, LUA_REGISTRYINDEX);
		if (lua_istable(L, -1))
		{
			lua_rawgeti(L, -1, 1);
			const bool same = lua_touserdata(L, -1) == (void*)p;
			lua_pop(L, 1);
			if (same)
			{
				lua_rawgeti(L, -1, 2);
				lua_remove(L, -2);
				return 1;
			}
		}
		lua_pop(L, 1);
		lua_createtable(L, 2, 0);
		lua_pushlightuserdata(L, (void*)p);
		lua_rawseti(L, -2, 1);
		luabind::detail::convert_to_lua(L, p);
		lua_pushvalue(L, -1);
		lua_rawseti(L, -3, 2);
		lua_pushlightuserdata(L, &key);
		lua_pushvalue(L, -3);
		lua_rawset(L, LUA_REGISTRYINDEX);
		lua_remove(L, -2);
		return 1;
	}
}

template <auto Getter>
void set_cached_singleton(lua_State* L, const char* name)
{
	lua_pushcfunction(L, &script_cached_singleton::call<Getter>);
	lua_setfield(L, LUA_GLOBALSINDEX, name);
}
