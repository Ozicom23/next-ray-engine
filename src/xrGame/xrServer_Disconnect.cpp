#include "stdafx.h"
#include "xrServer.h"
#include "file_transfer.h"
#include "screenshot_server.h"

void xrServer::Disconnect()
{
	if (m_file_transfers)
	{
		deinitialize_screenshot_proxies();
		xr_delete(m_file_transfers);
	}

	IPureServer::Disconnect();
	{
		PROF_EVENT("server: entities");
		SLS_Clear();
	}
	PROF_EVENT("server: game (A-Life)");
	xr_delete(game);
}
