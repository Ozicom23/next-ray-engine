#pragma once

// Profiler backends
#define PROFILER_NONE   (0)
#define PROFILER_OPTICK (1)

// Set active profiler backend
#if !defined(XRCORE_PROFILER)
    #define XRCORE_PROFILER PROFILER_NONE
//  #define XRCORE_PROFILER PROFILER_OPTICK
#endif

// Implement profiler macro interface
#if XRCORE_PROFILER == PROFILER_OPTICK
#	include "../3rd party/optick-git/src/optick.h"
#	define PROF_THREAD(Name) OPTICK_THREAD(Name)
#	define PROF_START_CAPTURE() OPTICK_START_CAPTURE()
#	define PROF_STOP_CAPTURE() OPTICK_STOP_CAPTURE()
#	define PROF_SAVE_CAPTURE(Name) OPTICK_SAVE_CAPTURE(Name)
#	define PROF_FRAME(Name) OPTICK_FRAME(Name)
#	define PROF_EVENT(Name) OPTICK_EVENT(Name)
#	define PROF_EVENT_DYNAMIC(...)	 static ::Optick::EventDescription* OPTICK_CONCAT(autogen_description_, __LINE__) = nullptr; \
							 OPTICK_CONCAT(autogen_description_, __LINE__) = ::Optick::CreateDescription(OPTICK_FUNC, __FILE__, __LINE__, ##__VA_ARGS__); \
							 ::Optick::Event OPTICK_CONCAT(autogen_event_, __LINE__)( *(OPTICK_CONCAT(autogen_description_, __LINE__)) ); 
#	define START_PROFILE(a) { PROF_EVENT(a)
#	define STOP_PROFILE		}
#else
// stat_cpu_passes (profiler_lite.h)
#	include "profiler_lite.h"
#	define START_PROFILE(a) { PROF_LITE_EVENT(a)
#	define STOP_PROFILE		}

#	define PROF_THREAD(Name)
#	define PROF_START_CAPTURE()
#	define PROF_STOP_CAPTURE()
#	define PROF_SAVE_CAPTURE(Name)
#	define PROF_FRAME(Name) ::xr_prof::frame()
#	define PROF_EVENT(...) PROF_LITE_EVENT(__VA_ARGS__)
#	define PROF_EVENT_DYNAMIC(...) {};
#endif