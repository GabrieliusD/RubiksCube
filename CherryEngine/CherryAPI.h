#pragma once

// Single export macro for CherryEngine.
//  - Building the engine DLL:      CHERRYENGINE_EXPORTS is defined  -> dllexport
//  - Consuming the engine DLL:     CHERRYENGINE_SHARED is defined   -> dllimport
//  - Static (monolithic) build:    neither is defined               -> nothing
#if defined(_WIN32)
	#if defined(CHERRYENGINE_EXPORTS)
		#define CHERRY_API __declspec(dllexport)
	#elif defined(CHERRYENGINE_SHARED)
		#define CHERRY_API __declspec(dllimport)
	#else
		#define CHERRY_API
	#endif
#else
	#define CHERRY_API
#endif

// Exported classes hold STL members; that's fine as long as engine and game
// are built with the same compiler and CRT, which the CMake setup guarantees.
#if defined(_MSC_VER)
	#pragma warning(disable : 4251)
#endif
