#pragma once

// stat_lua_sample <seconds>: where the time of A-Life's updates, of NPCs' and the actor's and of every scheduled object's
// updates goes. A thread of its
// own samples them about once a millisecond (the updating thread suspended for the moment of the sample): the object
// being updated and the Lua frames. At the end a report to logs\lua_sample_<time>.txt: time per object section and per
// Lua function (its own and with what it calls), the slowest stacks; for all updates and for the long ones. 0 ends it
// early.
// stat_luabind_objects 1 / 0: the luabind objects scripts make (each a userdata the Lua GC finalizes) by class, to the log
namespace lua_sampler
{
	enum kind
	{
		alife, // CALifeUpdateManager::update and update_left, on A-Life's thread
		npc,   // CAI_Stalker::shedule_Update, on the main thread
		actor, // CActor::shedule_Update (the actor binder's update: actor_on_update scripts), on the main thread
		scheduled, // every scheduled object's update (CSheduler::ProcessStep: smart terrains, NPCs, items...), by name
		kinds
	};

	void register_commands();
	// Sampling ended (its report written) and the thread joined: the level going away
	void shutdown();

	void update_begin(kind k);
	void update_end(kind k);
	// The object being updated (its section), nullptr after it; for NPCs given to update_begin's object
	void object(kind k, LPCSTR section);

	// An NPC's or the actor's update, timed while sampling
	struct scoped_update
	{
		const kind k;
		scoped_update(kind k, LPCSTR section) : k(k)
		{
			update_begin(k);
			object(k, section);
		}
		~scoped_update() { update_end(k); }
	};
}
