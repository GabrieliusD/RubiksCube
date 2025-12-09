#pragma once

// Simple macro to manage __declspec(dllexport/dllimport) for Windows DLL builds.
// When building CherryEngine as a shared lib, CMake defines CHERRYENGINE_EXPORTS.

#if defined(_WIN32)
  #if defined(CHERRYENGINE_EXPORTS)
    #define CHERRYENGINE_API __declspec(dllexport)
  #elif defined(CHERRYENGINE_SHARED)
    #define CHERRYENGINE_API __declspec(dllimport)
  #else
    #define CHERRYENGINE_API
  #endif
#else
  #if __GNUC__ >= 4
    #define CHERRYENGINE_API __attribute__ ((visibility ("default")))
  #else
    #define CHERRYENGINE_API
  #endif
#endif
