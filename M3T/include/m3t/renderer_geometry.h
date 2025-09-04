#ifndef M3T_INCLUDE_M3T_RENDERER_GEOMETRY_H_
#define M3T_INCLUDE_M3T_RENDERER_GEOMETRY_H_

#ifdef USE_EGL
  #include <m3t/renderer_geometry_egl.h>
#else
  #include <m3t/renderer_geometry_glfw.h>
#endif

#endif  // M3T_INCLUDE_M3T_RENDERER_GEOMETRY_H_
