#ifndef _EFFECTOR_BOBBING_H
#define _EFFECTOR_BOBBING_H
#pragma once

#include "CameraEffector.h"
#include "../xrEngine/cameramanager.h"

class CEffectorBobbing : public CEffectorCam
{
	float fTime;
	Fvector vAngleAmplitude;
	float fYAmplitude;
	float fSpeed;

	u32 dwMState;
	float fReminderFactor;
	bool is_limping;
	bool m_bZoomMode;
	float m_fPadWalks; // the gamepad's speed in walks (CActor::m_pad_walks), 0 with keys
	float m_fPadRunWalks;
	float fPhase; // the swing's, advanced by the rate
	float fCurAmplitude, fCurSpeed; // eased toward the state's

	float m_fAmplitudeRun;
	float m_fAmplitudeWalk;
	float m_fAmplitudeLimp;

	float m_fSpeedRun;
	float m_fSpeedWalk;
	float m_fSpeedLimp;

public:
	CEffectorBobbing();
	virtual ~CEffectorBobbing();
	virtual BOOL ProcessCam(SCamEffectorInfo& info);
	void SetState(u32 st, bool limping, bool ZoomMode, float pad_walks = 0.f, float pad_run_walks = 1.f);
};

#endif //_EFFECTOR_BOBBING_H

// demonized: First Person Death Cam Effector
class CFPCamEffector : public CEffectorCam
{
public:
	Fvector m_Position;
	Fvector m_HPB;
	Fmatrix m_Camera;
	bool hudEnabled = false;
	unsigned int m_customSmoothing; // 0 - use FPDeath smoothing params, no custom smoothing
	virtual void ema(Fvector& current, Fvector& target, unsigned int steps);
	
public:
	CFPCamEffector();
	virtual BOOL ProcessCam(SCamEffectorInfo& info);

};
