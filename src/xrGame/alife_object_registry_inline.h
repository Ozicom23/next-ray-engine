////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry_inline.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC void CALifeObjectRegistry::add(CSE_ALifeDynamicObject* object)
{
	if (objects().find(object->ID) != objects().end())
	{
		THROW2((*(objects().find(object->ID))).second == object,
		       "The specified object is already presented in the Object Registry!");
		THROW2((*(objects().find(object->ID))).second != object,
		       "Object with the specified ID is already presented in the Object Registry!");
	}

	if (m_objects.insert(std::make_pair(object->ID, object)).second)
		m_by_id[object->ID] = object;
}

IC void CALifeObjectRegistry::remove(const ALife::_OBJECT_ID& id, bool no_assert)
{
	OBJECT_REGISTRY::iterator I = m_objects.find(id);
	if (I == m_objects.end())
	{
		THROW2(no_assert, "The specified object hasn't been found in the Object Registry!");
		return;
	}

	m_objects.erase(I);
	m_by_id[id] = nullptr;
}

IC CSE_ALifeDynamicObject* CALifeObjectRegistry::object(const ALife::_OBJECT_ID& id, bool no_assert) const
{
	START_PROFILE("ALife/objects::object")
		CSE_ALifeDynamicObject* const found = m_by_id[id];

		if (!found)
		{
#ifdef DEBUG
		if (!no_assert)
			Msg					("There is no object with id %d!",id);
#endif
			THROW2(no_assert, "Specified object hasn't been found in the object registry!");
			return (0);
		}

		return found;
	STOP_PROFILE
}

IC const CALifeObjectRegistry::OBJECT_REGISTRY& CALifeObjectRegistry::objects() const
{
	return (m_objects);
}

IC CALifeObjectRegistry::OBJECT_REGISTRY& CALifeObjectRegistry::objects()
{
	return (m_objects);
}
