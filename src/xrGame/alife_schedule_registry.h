////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_schedule_registry.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife schedule registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "safe_map_iterator.h"
#include "xrServer_Objects_ALife.h"
#include "ai_debug.h"
#include "profiler.h"
#include "lua_sampler.h"

class CALifeScheduleRegistry : public CSafeMapIterator<
		ALife::_OBJECT_ID, CSE_ALifeSchedulable, std::less<ALife::_OBJECT_ID>, false>
{
private:
	struct CUpdatePredicate
	{
		u32 m_count;
		mutable u32 m_current;
		u64 m_deadline; // QPC: no object starts after it (one always does), 0 none
		mutable bool m_out_of_time;

		IC CUpdatePredicate(const u32& count, u64 deadline = 0)
		{
			m_count = count;
			m_current = 0;
			m_deadline = deadline;
			m_out_of_time = false;
		}

		IC bool operator()(_iterator& i, u64 cycle_count, bool) const
		{
			if ((*i).second->m_schedule_counter == cycle_count)
				return (false);

			if (m_current >= m_count)
				return (false);

			if (m_deadline && m_current && CPU::QPC() >= m_deadline)
			{
				m_out_of_time = true;
				return (false);
			}

			++m_current;
			(*i).second->m_schedule_counter = cycle_count;

			return (true);
		}

		IC void operator()(_iterator& i, u64 cycle_count) const
		{
			START_PROFILE("ALife/scheduled/update")
				const u64 t0 = CPU::QPC();
				CSE_Abstract* e = (*i).second->base();
				lua_sampler::object(lua_sampler::alife, e ? e->name() : nullptr);
				(*i).second->update();
				lua_sampler::object(lua_sampler::alife, nullptr);
				// A long one to the log (stat_frame_pacing): which object
				const double ms = double(CPU::QPC() - t0) * 1000.0 / double(CPU::qpc_freq);
				if (ms > 10.0 && g_bEnableStatGather && e)
					Msg("* [A-Life] scheduled %.1f ms: %s (%s)", ms, e->name_replace(), e->name());
			STOP_PROFILE
		}
	};

protected:
	typedef CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeSchedulable, std::less<ALife::_OBJECT_ID>, false> inherited;

protected:
	u32 m_objects_per_update;

public:
	IC CALifeScheduleRegistry();
	virtual ~CALifeScheduleRegistry();
	void add(CSE_ALifeDynamicObject* object);
	void remove(CSE_ALifeDynamicObject* object, bool no_assert = false);
	IC void update();
	// Up to count objects, none started after the deadline (QPC, 0 none): the number the deadline left, 0 when all
	// were done or the registry went round
	IC u32 update(u32 count, u64 deadline);
	IC CSE_ALifeSchedulable* object(const ALife::_OBJECT_ID& id, bool no_assert = false) const;
	IC const u32& objects_per_update() const;
	IC void objects_per_update(const u32& objects_per_update);
};

#include "alife_schedule_registry_inline.h"
