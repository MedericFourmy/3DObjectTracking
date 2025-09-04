// SPDX-License-Identifier: MIT
// Copyright (c) 2023 Manuel Stoiber, German Aerospace Center (DLR)

#include <m3t/renderer_geometry_egl.h>

namespace m3t {

int RendererGeometry::n_instances_ = 0;

RendererGeometry::RendererGeometry(const std::string &name) : name_{name} {}

RendererGeometry::~RendererGeometry() {
  if (initial_set_up_) {
    eglMakeCurrent(display_, surface_, surface_, context_);
    for (auto &render_data_body : render_data_bodies_) {
      DeleteGLVertexObjects(&render_data_body);
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    
    if (context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(display_, context_);
      context_ = EGL_NO_CONTEXT;
    }
    if (surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(display_, surface_);
      surface_ = EGL_NO_SURFACE;
    }
    if (display_ != EGL_NO_DISPLAY) {
      eglTerminate(display_);
      display_ = EGL_NO_DISPLAY;
    }
    
    n_instances_--;
  }
}

bool RendererGeometry::SetUp() {
  const std::lock_guard<std::mutex> lock{mutex_};
  set_up_ = false;

  // Check if all required objects are set up
  for (auto &body_ptr : body_ptrs_) {
    if (!body_ptr->set_up()) {
      std::cerr << "Body " << body_ptr->name() << " was not set up"
                << std::endl;
      return false;
    }
  }

  // Set up EGL
  if (!initial_set_up_) {
    // Get EGL display
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
      std::cerr << "Failed to get EGL display" << std::endl;
      return false;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(display_, &major, &minor)) {
      std::cerr << "Failed to initialize EGL" << std::endl;
      return false;
    }

    // Bind OpenGL API
    if (!eglBindAPI(EGL_OPENGL_API)) {
      std::cerr << "Failed to bind OpenGL API" << std::endl;
      eglTerminate(display_);
      display_ = EGL_NO_DISPLAY;
      return false;
    }

    // Choose EGL configuration
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(display_, config_attribs, &config, 1, &num_configs) || 
        num_configs == 0) {
      std::cerr << "Failed to choose EGL configuration" << std::endl;
      eglTerminate(display_);
      display_ = EGL_NO_DISPLAY;
      return false;
    }

    // Create EGL context with OpenGL 3.3 Core Profile
    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };

    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attribs);
    if (context_ == EGL_NO_CONTEXT) {
      std::cerr << "Failed to create EGL context" << std::endl;
      eglTerminate(display_);
      display_ = EGL_NO_DISPLAY;
      return false;
    }

    // Create a minimal pbuffer surface (since we don't need actual rendering output)
    EGLint surface_attribs[] = {
        EGL_WIDTH, 640,
        EGL_HEIGHT, 480,
        EGL_NONE
    };

    surface_ = eglCreatePbufferSurface(display_, config, surface_attribs);
    if (surface_ == EGL_NO_SURFACE) {
      std::cerr << "Failed to create EGL surface" << std::endl;
      eglDestroyContext(display_, context_);
      context_ = EGL_NO_CONTEXT;
      eglTerminate(display_);
      display_ = EGL_NO_DISPLAY;
      return false;
    }

    // Make OpengGL context current before loading OpenGL function pointers
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
      std::cerr << "Failed to make EGL context current" << std::endl;
      eglDestroySurface(display_, surface_);
      surface_ = EGL_NO_SURFACE;
      eglDestroyContext(display_, context_);
      context_ = EGL_NO_CONTEXT;
      eglTerminate(display_);
      display_ = EGL_NO_DISPLAY;
      return false;
    }

    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
      fprintf(stderr, "No valid OpenGL context\n");
    }

    // Load OpenGL function pointers
    if (!gladLoadGL()) {
      std::cerr << "Failed loading OpenGL function pointers with gladLoadGL" << std::endl;
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroySurface(display_, surface_);
      surface_ = EGL_NO_SURFACE;
      eglDestroyContext(display_, context_);
      context_ = EGL_NO_CONTEXT;
      eglTerminate(display_);
      display_ = EGL_NO_DISPLAY;
      return false;
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    n_instances_++;
    initial_set_up_ = true;
  }

  // Set up bodies
  eglMakeCurrent(display_, surface_, surface_, context_);
  for (auto &render_data_body : render_data_bodies_) {
    // Assemble vertex data
    std::vector<float> vertex_data;
    AssembleVertexData(*render_data_body.body_ptr, &vertex_data);
    render_data_body.n_vertices = unsigned(vertex_data.size()) / 6;

    // Create GL Vertex objects
    if (set_up_) DeleteGLVertexObjects(&render_data_body);
    CreateGLVertexObjects(vertex_data, &render_data_body);
  }
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  set_up_ = true;
  return true;
}

bool RendererGeometry::AddBody(const std::shared_ptr<Body> &body_ptr) {
  const std::lock_guard<std::mutex> lock{mutex_};

  // Check if renderer geometry for body already exists
  for (auto &p : body_ptrs_) {
    if (body_ptr->name() == p->name()) {
      std::cerr << "Body data " << body_ptr->name() << " already exists"
                << std::endl;
      return false;
    }
  }

  // Create data for body and assign parameters
  RenderDataBody render_data_body;
  render_data_body.body_ptr = body_ptr.get();
  if (set_up_ && body_ptr->set_up()) {
    // Assemble vertex data
    std::vector<float> vertex_data;
    AssembleVertexData(*body_ptr.get(), &vertex_data);
    render_data_body.n_vertices = unsigned(vertex_data.size()) / 6;

    // Create GL Vertex objects
    eglMakeCurrent(display_, surface_, surface_, context_);
    CreateGLVertexObjects(vertex_data, &render_data_body);
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  } else if (set_up_ && !body_ptr->set_up()) {
    set_up_ = false;
  }

  // Add body ptr and body data
  body_ptrs_.push_back(body_ptr);
  render_data_bodies_.push_back(std::move(render_data_body));
  return true;
}

bool RendererGeometry::DeleteBody(const std::string &name) {
  const std::lock_guard<std::mutex> lock{mutex_};
  for (size_t i = 0; i < body_ptrs_.size(); ++i) {
    if (name == body_ptrs_[i]->name()) {
      body_ptrs_.erase(begin(body_ptrs_) + i);
      if (set_up_) {
        eglMakeCurrent(display_, surface_, surface_, context_);
        DeleteGLVertexObjects(&render_data_bodies_[i]);
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      }
      render_data_bodies_.erase(begin(render_data_bodies_) + i);
      return true;
    }
  }
  std::cerr << "Body data \"" << name << "\" not found" << std::endl;
  return false;
}

void RendererGeometry::ClearBodies() {
  const std::lock_guard<std::mutex> lock{mutex_};
  if (set_up_) {
    eglMakeCurrent(display_, surface_, surface_, context_);
    for (auto &render_data_body : render_data_bodies_) {
      DeleteGLVertexObjects(&render_data_body);
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  render_data_bodies_.clear();
  body_ptrs_.clear();
}

bool RendererGeometry::MakeContextCurrent() {
  mutex_.lock();
  if (!initial_set_up_) {
    std::cerr << "Set up renderer geometry " << name_ << " first" << std::endl;
    mutex_.unlock();
    return false;
  }
  if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
    std::cerr << "Failed to make EGL context current" << std::endl;
    mutex_.unlock();
    return false;
  }
  return true;
}

bool RendererGeometry::DetachContext() {
  if (!initial_set_up_) {
    std::cerr << "Set up renderer geometry " << name_ << " first" << std::endl;
    return false;
  }
  if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
    std::cerr << "Failed to detach EGL context" << std::endl;
    mutex_.unlock();
    return false;
  }
  mutex_.unlock();
  return true;
}

const std::string &RendererGeometry::name() const { return name_; }

const std::vector<std::shared_ptr<Body>> &RendererGeometry::body_ptrs() const {
  return body_ptrs_;
}

const std::vector<RendererGeometry::RenderDataBody>
    &RendererGeometry::render_data_bodies() const {
  return render_data_bodies_;
}

bool RendererGeometry::set_up() const { return set_up_; }

void RendererGeometry::AssembleVertexData(const Body &body,
                                          std::vector<float> *vertex_data) {
  for (const auto &triangle_indices : body.mesh_indices()) {
    std::array<Eigen::Vector3f, 3> points;
    for (int i = 0; i < 3; ++i)
      points[i] = body.vertices()[triangle_indices[i]];

    Eigen::Vector3f normal{
        (points[2] - points[1]).cross(points[0] - points[1]).normalized()};

    for (auto point : points) {
      vertex_data->insert(end(*vertex_data), point.data(), point.data() + 3);
      vertex_data->insert(end(*vertex_data), normal.data(), normal.data() + 3);
    }
  }
}

void RendererGeometry::CreateGLVertexObjects(const std::vector<float> &vertices,
                                             RenderDataBody *render_data_body) {
  glGenVertexArrays(1, &render_data_body->vao);
  glBindVertexArray(render_data_body->vao);

  glGenBuffers(1, &render_data_body->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, render_data_body->vbo);
  glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
               &vertices.front(), GL_STATIC_DRAW);

  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                        (void *)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

void RendererGeometry::DeleteGLVertexObjects(RenderDataBody *render_data_body) {
  glDeleteBuffers(1, &render_data_body->vbo);
  glDeleteVertexArrays(1, &render_data_body->vao);
}

}  // namespace m3t
