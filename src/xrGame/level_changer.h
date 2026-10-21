////////////////////////////////////////////////////////////////////////////
//	Module 		: level_changer.h
//	Created 	: 10.07.2003
//  Modified 	: 10.07.2003
//	Author		: Dmitriy Iassenev
//	Description : Level change object
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "GameObject.h"
#include "../xrEngine/feel_touch.h"
#include "game_graph_space.h"

// The travel request of a level changer (M_CHANGE_LEVEL); with fade_transitions after the screen faded to black
void send_change_level(GameGraph::_GRAPH_ID game_vertex_id, u32 level_vertex_id, const Fvector& position,
                       const Fvector& angles);

class CLevelChanger : public CGameObject, public Feel::Touch
{
private:
	typedef CGameObject inherited;

private:
	GameGraph::_GRAPH_ID m_game_vertex_id;
	u32 m_level_vertex_id;
	Fvector m_position;
	Fvector m_angles;
	float m_entrance_time;
	shared_str m_invite_str;
	bool m_b_enabled;

	void update_actor_invitation();
	bool m_bSilentMode;
	bool get_reject_pos(Fvector& p, Fvector& r);

public:
	virtual ~CLevelChanger();
	virtual BOOL net_Spawn(CSE_Abstract* DC);
	virtual void net_Destroy();
	virtual void Center(Fvector& C) const;
	virtual float Radius() const;
	virtual void shedule_Update(u32 dt);
	virtual void feel_touch_new(CObject* O);
	virtual bool feel_touch_contact(CObject* O);

	virtual bool IsVisibleForZones() { return false; }
	void EnableLevelChanger(bool b) { m_b_enabled = b; }
	bool IsLevelChangerEnabled() const { return m_b_enabled; }
	// The level it leads to (its folder name), nullptr when the game graph doesn't know it
	LPCSTR target_level() const;
	void SetLEvelChangerInvitationStr(LPCSTR str) { m_invite_str = str; }
	//serialization
	virtual BOOL net_SaveRelevant();
	virtual void save(NET_Packet& output_packet);
	virtual void load(IReader& input_packet);

	virtual CGameObject* cast_game_object() { return this; }
	virtual CLevelChanger* cast_level_changer() { return this; }
};
