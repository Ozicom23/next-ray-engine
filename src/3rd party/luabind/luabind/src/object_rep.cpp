// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.
#include "luabind_api.h"
#include <cstring>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include <luabind/detail/class_rep.hpp>
#include <luabind/detail/object_rep.hpp>
#include <luabind/detail/method_rep.hpp>

#include <luabind/detail/overload_rep_impl.hpp>

//#define TEST_GARBAGE_COLLECTOR

#ifdef TEST_GARBAGE_COLLECTOR
#	include "pch.h"
	string256	debug_test_constructor, debug_test_destructor;
	const char*		debug_class = "AttackError";//"stateAttackThreaten";
#endif

namespace luabind { namespace detail
{
	// stat_luabind_objects: an open hash by class (Lua runs on one thread at a time)
	static bool s_count_on = false;
	static const unsigned kCountSlots = 1024;
	static struct { const class_rep* crep; unsigned made, collected; } s_counts[kCountSlots];

	static void count(const class_rep* crep, bool made)
	{
		unsigned i = unsigned((reinterpret_cast<uintptr_t>(crep) >> 4) % kCountSlots);
		for (unsigned n = 0; n < kCountSlots; ++n, i = (i + 1) % kCountSlots)
		{
			if (s_counts[i].crep == crep || !s_counts[i].crep)
			{
				s_counts[i].crep = crep;
				++(made ? s_counts[i].made : s_counts[i].collected);
				return;
			}
		}
	}

	void xr_count_objects(bool on)
	{
		if (on)
			memset(s_counts, 0, sizeof(s_counts));
		s_count_on = on;
	}

	void xr_object_counts(void (*row)(const char* name, unsigned made, unsigned collected, void* ud), void* ud)
	{
		for (unsigned i = 0; i < kCountSlots; ++i)
			if (s_counts[i].crep)
				row(s_counts[i].crep->name(), s_counts[i].made, s_counts[i].collected, ud);
	}

	// registry[&s_cache_key] = {[lightuserdata pointer] = object}; the pointers of destroyed objects wait in s_dead
	static char s_cache_key;
	static std::mutex s_dead_lock;
	static std::vector<void*> s_dead;
	static std::atomic<bool> s_dead_any{false};

	// Pushes the cache table, without the entries of objects destroyed since
	static void push_cache(lua_State* L)
	{
		lua_pushlightuserdata(L, &s_cache_key);
		lua_rawget(L, LUA_REGISTRYINDEX);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			lua_newtable(L);
			lua_pushlightuserdata(L, &s_cache_key);
			lua_pushvalue(L, -2);
			lua_rawset(L, LUA_REGISTRYINDEX);
		}
		if (!s_dead_any.load(std::memory_order_acquire))
			return;
		std::vector<void*> dead;
		{
			std::lock_guard<std::mutex> lock(s_dead_lock);
			dead.swap(s_dead);
			s_dead_any.store(false, std::memory_order_relaxed);
		}
		for (void* ptr : dead)
		{
			lua_pushlightuserdata(L, ptr);
			lua_pushnil(L);
			lua_rawset(L, -3);
		}
	}

	bool xr_pointer_cache_push(lua_State* L, void* ptr, const class_rep* crep)
	{
		push_cache(L);
		lua_pushlightuserdata(L, ptr);
		lua_rawget(L, -2);
		const object_rep* obj = static_cast<const object_rep*>(lua_touserdata(L, -1));
		if (obj && obj->crep() == crep && obj->flags() == 0)
		{
			lua_remove(L, -2);
			return true;
		}
		lua_pop(L, 2);
		return false;
	}

	void xr_pointer_cache_store(lua_State* L, void* ptr)
	{
		push_cache(L);
		lua_pushlightuserdata(L, ptr);
		lua_pushvalue(L, -3);
		lua_rawset(L, -3);
		lua_pop(L, 1);
	}

	void xr_pointer_cache_forget(void* ptr)
	{
		std::lock_guard<std::mutex> lock(s_dead_lock);
		s_dead.push_back(ptr);
		s_dead_any.store(true, std::memory_order_release);
	}

	// dest is a function that is called to delete the c++ object this struct holds
	object_rep::object_rep(void* obj, class_rep* crep, int flags, void(*dest)(void*))
		: m_object(obj)
		, m_classrep(crep)
		, m_flags(flags)
		, m_destructor(dest)
		, m_dependency_cnt(1)
	{
		if (s_count_on)
			count(crep, true);
		// if the object is owned by lua, a valid destructor must be given
		assert((((m_flags & owner) && dest) || !(m_flags & owner)) && "internal error, please report");
#ifdef TEST_GARBAGE_COLLECTOR
		sprintf(debug_test_constructor,"%s %x %x",m_classrep->name(),m_object,this);
		if (!sz_cmp(debug_class,m_classrep->name()))
			Log(debug_test_constructor);
#endif
	}

	object_rep::object_rep(class_rep* crep, int flags, detail::lua_reference const& table_ref)
		: m_object(0)
		, m_classrep(crep)
		, m_flags(flags)
		, m_lua_table_ref(table_ref)
		, m_destructor(0)
		, m_dependency_cnt(1)
	{
		if (s_count_on)
			count(crep, true);
#ifdef TEST_GARBAGE_COLLECTOR
		sprintf(debug_test_constructor,"%s %x %x",m_classrep->name(),m_object,this);
		if (!sz_cmp(debug_class,m_classrep->name()))
			Log(debug_test_constructor);
#endif
	}

	object_rep::~object_rep() 
	{
#ifdef TEST_GARBAGE_COLLECTOR
		sprintf(debug_test_destructor,"%s %x %x",m_classrep->name(),m_object,this);
		if (!sz_cmp(debug_class,m_classrep->name()))
			Log(debug_test_destructor);
#endif
		if (m_flags & owner && m_destructor) m_destructor(m_object);
	}

	void object_rep::remove_ownership()
	{
		assert((m_flags & owner) && "cannot remove ownership of object that's not owned");
		assert(m_classrep->get_class_type() == class_rep::cpp_class
			|| m_classrep->bases().size() == 1 && "can only adopt c++ types or lua classes that derives from a c++ class");

        // daniel040727 Bogus assert above? C++ types can be adopted just fine
        // without a hierarchy?

		m_flags &= ~owner;
		// if this is a type with a wrapper we also have to
		// transform the wrappers weak_ref into a strong
		// reference, to make sure the lua part
		// stays alive as long as the c++ part stays
		// alive.
/*		if (m_classrep->get_class_type() == class_rep::cpp_class)
			m_classrep->adopt(m_flags & constant, m_object);
		else*/

        // daniel040727 I changed the above. It just seems wrong. adopt()
        // should only be called on the wrapper type.
        if (m_classrep->get_class_type() == class_rep::lua_class)
			m_classrep->bases().front().base->adopt(m_flags & constant, m_object);
	}

	void object_rep::set_destructor(void(*ptr)(void*))
	{
		m_destructor = ptr;
	}

	void object_rep::add_dependency(lua_State* L, int index)
	{
		if (!m_dependency_ref.is_valid())
		{
			lua_newtable(L);
			m_dependency_ref.set(L);
		}

		m_dependency_ref.get(L);
		lua_pushvalue(L, index);
		lua_rawseti(L, -2, m_dependency_cnt);
		lua_pop(L, 1);
		++m_dependency_cnt;
	}

	int object_rep::garbage_collector(lua_State* L)
	{
		object_rep* obj = static_cast<object_rep*>(lua_touserdata(L, -1));

		if (s_count_on)
			count(obj->crep(), false);
		finalize(L, obj->crep());

		obj->~object_rep();
		return 0;
	}

}}

