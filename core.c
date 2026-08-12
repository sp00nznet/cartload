// libretro host for DeckEmu.
//
// The core renders with OpenGL into an FBO we own, then we read the pixels back to
// the CPU and hand them to the UI as a plain buffer.
//
// ponytail: readback (~1ms at 640x480 on an APU, unified memory) buys a completely
// separate GL context for the core, so its state can't corrupt SDL_Renderer's and the
// menu/toast overlay keeps working exactly like SnesDeck's. If that ever shows up in
// the frame budget, share one context and glBlitFramebuffer instead.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "libretro.h"

// GL 3.0 framebuffer objects: not in the 1.1 opengl32 import lib, so load them by hand
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED

typedef void(APIENTRY* PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_BindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY* PFN_DeleteFramebuffers)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void(APIENTRY* PFN_GenRenderbuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_BindRenderbuffer)(GLenum, GLuint);
typedef void(APIENTRY* PFN_DeleteRenderbuffers)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFN_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void(APIENTRY* PFN_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum(APIENTRY* PFN_CheckFramebufferStatus)(GLenum);
typedef void(APIENTRY* PFN_BindBuffer)(GLenum, GLuint);

static struct {
  PFN_GenFramebuffers GenFramebuffers;
  PFN_BindFramebuffer BindFramebuffer;
  PFN_DeleteFramebuffers DeleteFramebuffers;
  PFN_FramebufferTexture2D FramebufferTexture2D;
  PFN_GenRenderbuffers GenRenderbuffers;
  PFN_BindRenderbuffer BindRenderbuffer;
  PFN_DeleteRenderbuffers DeleteRenderbuffers;
  PFN_RenderbufferStorage RenderbufferStorage;
  PFN_FramebufferRenderbuffer FramebufferRenderbuffer;
  PFN_CheckFramebufferStatus CheckFramebufferStatus;
  PFN_BindBuffer BindBuffer;
} gl;

// core entry points, loaded from the dll
static struct {
  void (*init)(void);
  void (*deinit)(void);
  unsigned (*api_version)(void);
  void (*get_system_info)(struct retro_system_info*);
  void (*get_system_av_info)(struct retro_system_av_info*);
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample)(retro_audio_sample_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*set_controller_port_device)(unsigned, unsigned);
  void (*reset)(void);
  void (*run)(void);
  size_t (*serialize_size)(void);
  bool (*serialize)(void*, size_t);
  bool (*unserialize)(const void*, size_t);
  bool (*load_game)(const struct retro_game_info*);
  void (*unload_game)(void);
} r;

// What the caller wants different from the core's own defaults -- see systems.c for the
// per-system tables. Everything else is answered from what the core declares in
// SET_VARIABLES, below.
static const char* const (*pinned)[2];
static int pinnedCount;

// The core declares its settings once, each as "Description; default|other|other".
// Serving those defaults back is the frontend's job: a core that gets no answer keeps
// whatever its variable happened to be initialised to, and GLideN64 sizes its texture
// caches from several of them.
#define MAX_OPTIONS 256
static struct {
  char key[64];
  char value[64];
} declared[MAX_OPTIONS];
static int declaredCount;

static struct {
  void* lib;
  SDL_Window* window;
  SDL_GLContext uiCtx, coreCtx;
  struct retro_hw_render_callback hw;
  bool hwReady;

  GLuint fbo, colorTex, depthRb;
  int fbW, fbH;       // fbo size, the core's declared maximum
  int frameW, frameH; // what the last frame actually used
  uint8_t* pixels;

  SDL_AudioDeviceID audio;
  int audioRate;
  int maxQueue;

  enum retro_pixel_format swFormat;
  bool software; // core handed us pixels directly instead of rendering into the fbo

  float frameTime;
  float aspect;
  bool needFullpath;
  bool gameLoaded;
  char systemDir[512], saveDir[512];
  char err[256]; // set from inside the environment callback, which has nowhere to return one
} c;

static bool makeContext(char* err, size_t errLen);

uint8_t corePad[16];
int16_t coreStick[2];
int16_t coreCStick[2];

// --- callbacks the core calls into ------------------------------------------

bool coreVerbose;

static void logCb(enum retro_log_level level, const char* fmt, ...) {
  if(level < RETRO_LOG_WARN && !coreVerbose) return; // the core is chatty at INFO
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  SDL_Log("core: %s", line);
}

static uintptr_t getFramebufferCb(void) {
  return c.fbo;
}

static retro_proc_address_t getProcCb(const char* sym) {
  return (retro_proc_address_t) SDL_GL_GetProcAddress(sym);
}

static bool envImpl(unsigned cmd, void* data);

static bool envCb(unsigned cmd, void* data) {
  bool ok = envImpl(cmd, data);
  if(coreVerbose) SDL_Log("env %u%s -> %s", cmd & 0xFFFF, (cmd & 0x10000) ? " (exp)" : "", ok ? "true" : "FALSE");
  return ok;
}

static bool envImpl(unsigned cmd, void* data) {
  switch(cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *(bool*) data = true;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      // all three: 0RGB1555 is what the older cores in the table still hand back
      c.swFormat = *(const enum retro_pixel_format*) data; // only matters on the software path
      return true;
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
      *(uint64_t*) data = (1 << RETRO_DEVICE_JOYPAD) | (1 << RETRO_DEVICE_ANALOG);
      return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
      *(const char**) data = c.systemDir;
      return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      // the core writes its own .sra/.eep/.fla here, so battery saves need no code of ours
      *(const char**) data = c.saveDir;
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback*) data)->log = logCb;
      return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
      *(unsigned*) data = RETRO_LANGUAGE_ENGLISH;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable* v = data;
      v->value = NULL;
      for(int i = 0; i < pinnedCount; i++) {
        if(strcmp(v->key, pinned[i][0]) == 0) v->value = pinned[i][1];
      }
      for(int i = 0; v->value == NULL && i < declaredCount; i++) {
        if(strcmp(v->key, declared[i].key) == 0) v->value = declared[i].value;
      }
      return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool*) data = false; // our overrides never change mid-run
      return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
      const struct retro_variable* v = data;
      declaredCount = 0;
      for(; v != NULL && v->key != NULL && declaredCount < MAX_OPTIONS; v++) {
        const char* semi = v->value != NULL ? strchr(v->value, ';') : NULL;
        const char* def = semi != NULL ? semi + 1 : v->value;
        if(def == NULL) continue;
        while(*def == ' ') def++;
        size_t n = strcspn(def, "|"); // first choice listed is the default
        if(n == 0 || n >= sizeof(declared[0].value)) continue;
        snprintf(declared[declaredCount].key, sizeof(declared[0].key), "%s", v->key);
        snprintf(declared[declaredCount].value, sizeof(declared[0].value), "%.*s", (int) n, def);
        if(coreVerbose) SDL_Log("opt %s = %s", declared[declaredCount].key, declared[declaredCount].value);
        declaredCount++;
      }
      return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: // res only shrinks within the fbo we made
      return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      struct retro_hw_render_callback* hw = data;
      if(hw->context_type != RETRO_HW_CONTEXT_OPENGL && hw->context_type != RETRO_HW_CONTEXT_OPENGL_CORE) return false;
      if(coreVerbose) {
        SDL_Log("hw_render type %u ver %u.%u depth %d stencil %d bottomleft %d", hw->context_type,
                hw->version_major, hw->version_minor, (int) hw->depth, (int) hw->stencil,
                (int) hw->bottom_left_origin);
      }
      c.hw = *hw;
      // the core reads these back out of the struct it handed us
      hw->get_current_framebuffer = getFramebufferCb;
      hw->get_proc_address = getProcCb;
      c.hw.get_current_framebuffer = getFramebufferCb;
      c.hw.get_proc_address = getProcCb;
      return makeContext(c.err, sizeof(c.err));
    }
    default: return false; // core options v1/v2, vfs, rumble, perf: legacy paths are fine
  }
}

static void readbackFrame(int width, int height);

// The readback has to happen here and not after r.run() returns: GLideN64 hands the
// frame over at swap time and then carries straight on into the next one, so by the
// time run() is done the colour attachment has already been cleared. Deferring it
// reads back a black screen.
static void videoCb(const void* data, unsigned width, unsigned height, size_t pitch) {
  if(data == NULL) return; // "same frame again"
  if(data == RETRO_HW_FRAME_BUFFER_VALID) {
    readbackFrame((int) width, (int) height);
    return;
  }
  // software renderer (angrylion): the frame is already in main memory, just row-copy it
  // in. Rows can be padded, so pitch is not width.
  size_t bpp = c.swFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2; // 565 and 0RGB1555 are both 16bpp
  if((int) width > c.fbW || (int) height > c.fbH || c.pixels == NULL) return;
  for(unsigned y = 0; y < height; y++) {
    memcpy(c.pixels + (size_t) y * width * bpp, (const uint8_t*) data + y * pitch, width * bpp);
  }
  c.frameW = (int) width;
  c.frameH = (int) height;
  c.software = true;
}

static void readbackFrame(int width, int height) {
  if(width > c.fbW) width = c.fbW;
  if(height > c.fbH) height = c.fbH;
  gl.BindFramebuffer(GL_FRAMEBUFFER, c.fbo);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  // GLideN64 reads its own framebuffers back through a pixel pack buffer and leaves the
  // pack state set. Ours is a plain client pointer: with a PBO still bound the pointer
  // would be read as an offset into it, and a stale row length would stride us straight
  // off the end of c.pixels. Neither is ours to inherit, so pin the state and hand it back.
  GLint pbo = 0, rowLength = 0, skipPixels = 0, skipRows = 0, alignment = 4;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
  glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
  glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
  glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
  if(pbo != 0) gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 4); // rgba8 rows are 4 byte multiples anyway

  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, c.pixels);

  if(pbo != 0) gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint) pbo);
  glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
  glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
  glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  c.frameW = width;
  c.frameH = height;
}

static size_t audioBatchCb(const int16_t* data, size_t frames) {
  // drop rather than grow an unbounded queue: a stutter beats seconds of lag
  if(c.audio != 0 && (int) SDL_GetQueuedAudioSize(c.audio) < c.maxQueue) {
    SDL_QueueAudio(c.audio, data, (Uint32) (frames * 4));
  }
  return frames;
}

static void audioCb(int16_t left, int16_t right) {
  int16_t f[2] = {left, right};
  audioBatchCb(f, 1);
}

static void pollCb(void) {} // main.c already pumped SDL events this frame

static int16_t inputCb(unsigned port, unsigned device, unsigned index, unsigned id) {
  if(port != 0) return 0;
  if(device == RETRO_DEVICE_JOYPAD) return id < 16 ? corePad[id] : 0;
  if(device == RETRO_DEVICE_ANALOG && index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
    const int16_t* s = (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) ? coreCStick : coreStick;
    return (id == RETRO_DEVICE_ID_ANALOG_X) ? s[0] : s[1];
  }
  return 0;
}

// --- setup -----------------------------------------------------------------

static bool loadSymbols(char* err, size_t errLen) {
#define SYM(name)                                                       \
  *(void**) &r.name = SDL_LoadFunction(c.lib, "retro_" #name);          \
  if(r.name == NULL) {                                                  \
    snprintf(err, errLen, "core is missing retro_%s", #name);           \
    return false;                                                       \
  }
  SYM(init) SYM(deinit) SYM(api_version) SYM(get_system_info) SYM(get_system_av_info)
  SYM(set_environment) SYM(set_video_refresh) SYM(set_audio_sample) SYM(set_audio_sample_batch)
  SYM(set_input_poll) SYM(set_input_state) SYM(set_controller_port_device) SYM(reset) SYM(run)
  SYM(serialize_size) SYM(serialize) SYM(unserialize) SYM(load_game) SYM(unload_game)
#undef SYM
  return true;
}

// The core asks for its GL context during retro_load_game, before we know the frame size,
// so context and framebuffer are made in two steps.
static bool makeContext(char* err, size_t errLen) {
  if(c.coreCtx != NULL) return true;
  SDL_GL_ResetAttributes();
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0); // isolated from SDL_Renderer's state
  // Version 0.0 means "whatever you have". Asking for a specific 3.3 context instead gets
  // one that reports exactly 3.3, and GLideN64 then takes an older, far less travelled
  // path. Leaving the attributes alone makes SDL create a plain legacy context -- the
  // driver's newest -- which is what every other libretro frontend hands it.
  if(c.hw.version_major != 0) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, (int) c.hw.version_major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, (int) c.hw.version_minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, c.hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE
                                                         ? SDL_GL_CONTEXT_PROFILE_CORE
                                                         : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  }
  c.coreCtx = SDL_GL_CreateContext(c.window);
  if(c.coreCtx == NULL) {
    snprintf(err, errLen, "no OpenGL context for the core: %s", SDL_GetError());
    return false;
  }
#define GLSYM(name)                                            \
  gl.name = (PFN_##name) SDL_GL_GetProcAddress("gl" #name);    \
  if(gl.name == NULL) {                                        \
    snprintf(err, errLen, "OpenGL is missing gl" #name);        \
    SDL_GL_MakeCurrent(c.window, c.uiCtx);                     \
    return false;                                              \
  }
  GLSYM(GenFramebuffers) GLSYM(BindFramebuffer) GLSYM(DeleteFramebuffers) GLSYM(FramebufferTexture2D)
  GLSYM(GenRenderbuffers) GLSYM(BindRenderbuffer) GLSYM(DeleteRenderbuffers) GLSYM(RenderbufferStorage)
  GLSYM(FramebufferRenderbuffer) GLSYM(CheckFramebufferStatus) GLSYM(BindBuffer)
#undef GLSYM
  // deliberately left current: the core keeps initialising GL for the rest of
  // retro_load_game, and it must see its own context when it does
  return true;
}

static bool makeFramebuffer(int w, int h, char* err, size_t errLen) {
  c.fbW = w;
  c.fbH = h;
  free(c.pixels);
  c.pixels = calloc((size_t) w * h, 4);
  gl.GenFramebuffers(1, &c.fbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, c.fbo);
  glGenTextures(1, &c.colorTex);
  glBindTexture(GL_TEXTURE_2D, c.colorTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.colorTex, 0);
  if(c.hw.depth) { // one combined buffer whether or not it asked for stencil too
    gl.GenRenderbuffers(1, &c.depthRb);
    gl.BindRenderbuffer(GL_RENDERBUFFER, c.depthRb);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c.depthRb);
  }
  if(gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || c.pixels == NULL) {
    snprintf(err, errLen, "could not create a %dx%d render target", w, h);
    return false;
  }
  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  {
    uint8_t probe[4] = {0, 0, 0, 0};
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, probe);
    SDL_Log("fbo %dx%d probe %02x%02x%02x%02x glerr %x", w, h, probe[0], probe[1], probe[2], probe[3], glGetError());
  }
  return true;
}

bool coreOpen(const char* dllPath, SDL_Window* window, const char* systemDir, const char* saveDir,
              const char* const (*options)[2], int optionCount, char* err, size_t errLen) {
  coreClose(); // a previous system's core, if any: one at a time
  memset(&c, 0, sizeof(c));
  declaredCount = 0;
  pinned = options;
  pinnedCount = optionCount;
  c.window = window;
  c.uiCtx = SDL_GL_GetCurrentContext(); // SDL_Renderer's own context, ours to hand back to
  if(c.uiCtx == NULL) {
    snprintf(err, errLen, "the renderer is not using OpenGL, so the core has nothing to draw into");
    return false;
  }
  snprintf(c.systemDir, sizeof(c.systemDir), "%s", systemDir);
  snprintf(c.saveDir, sizeof(c.saveDir), "%s", saveDir);
  c.lib = SDL_LoadObject(dllPath);
  if(c.lib == NULL) {
    snprintf(err, errLen, "%s", SDL_GetError());
    return false;
  }
  if(!loadSymbols(err, errLen)) return false;
  if(r.api_version() != RETRO_API_VERSION) {
    snprintf(err, errLen, "core speaks libretro %u, this build needs %u", r.api_version(), RETRO_API_VERSION);
    return false;
  }
  r.set_environment(envCb);
  r.set_video_refresh(videoCb);
  r.set_audio_sample(audioCb);
  r.set_audio_sample_batch(audioBatchCb);
  r.set_input_poll(pollCb);
  r.set_input_state(inputCb);
  {
    // Disc-based and computer cores read the image themselves, off the path, rather
    // than take it in memory: PPSSPP, flycast, MAME and DOSBox all want a real file.
    struct retro_system_info info;
    memset(&info, 0, sizeof(info));
    r.get_system_info(&info);
    c.needFullpath = info.need_fullpath;
  }
  r.init();
  return true;
}

bool coreIsOpen(void) {
  return c.lib != NULL;
}

bool coreNeedFullpath(void) {
  return c.needFullpath;
}

const char* coreName(void) {
  static char name[128];
  if(c.lib == NULL) return NULL;
  struct retro_system_info info;
  memset(&info, 0, sizeof(info));
  r.get_system_info(&info);
  if(info.library_name == NULL) return NULL;
  snprintf(name, sizeof(name), "%s %s", info.library_name, info.library_version ? info.library_version : "");
  return name;
}

// `data` is the rom already in memory (unpacked from an archive, say). NULL means read
// `path` here, and a need_fullpath core is handed the path alone either way.
bool coreLoadGame(const char* path, const void* data, size_t size, char* err, size_t errLen) {
  coreUnloadGame();
  void* owned = NULL;
  if(!c.needFullpath && data == NULL) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
      snprintf(err, errLen, "could not open the file");
      return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    owned = len > 0 ? malloc((size_t) len) : NULL;
    if(owned == NULL || fread(owned, (size_t) len, 1, f) != 1) {
      free(owned);
      fclose(f);
      snprintf(err, errLen, "could not read the rom");
      return false;
    }
    fclose(f);
    data = owned;
    size = (size_t) len;
  }

  struct retro_game_info info;
  memset(&info, 0, sizeof(info));
  info.path = path;
  if(!c.needFullpath) { // the core keeps its own copy, so the buffer is ours to free after
    info.data = data;
    info.size = size;
  }
  c.err[0] = '\0';
  bool ok = r.load_game(&info);
  free(owned);
  if(!ok) {
    snprintf(err, errLen, "%s", c.err[0] ? c.err : "the core would not take that rom");
    return false;
  }

  struct retro_system_av_info av;
  memset(&av, 0, sizeof(av));
  r.get_system_av_info(&av);
  c.frameTime = av.timing.fps > 1.0 ? (float) (1.0 / av.timing.fps) : 1.0f / 60.0f;
  c.aspect = av.geometry.aspect_ratio > 0.1f ? av.geometry.aspect_ratio : 0.0f;
  if(c.aspect == 0.0f && av.geometry.base_height > 0) {
    c.aspect = (float) av.geometry.base_width / (float) av.geometry.base_height;
  }

  c.software = false;
  if(c.coreCtx != NULL) {
    // square power of two, which is what RetroArch hands hw cores. Sizing it to the
    // geometry exactly appears to work too, but there is no upside worth the risk of
    // being the only frontend a core has never been run against.
    int side = 1;
    while(side < (int) av.geometry.max_width || side < (int) av.geometry.max_height) side <<= 1;
    SDL_GL_MakeCurrent(c.window, c.coreCtx);
    if(!makeFramebuffer(side, side, err, errLen)) {
      SDL_GL_MakeCurrent(c.window, c.uiCtx);
      r.unload_game();
      return false;
    }
    if(c.hw.context_reset) c.hw.context_reset();
    c.hwReady = true;
    SDL_GL_MakeCurrent(c.window, c.uiCtx);
  } else { // software renderer: no gl at all, the core writes pixels straight to us
    c.fbW = (int) av.geometry.max_width;
    c.fbH = (int) av.geometry.max_height;
    free(c.pixels);
    c.pixels = calloc((size_t) c.fbW * c.fbH, 4);
    if(c.pixels == NULL) {
      snprintf(err, errLen, "out of memory for a %dx%d frame", c.fbW, c.fbH);
      r.unload_game();
      return false;
    }
  }

  c.audioRate = (int) (av.timing.sample_rate > 0 ? av.timing.sample_rate : 44100);
  if(c.audio == 0) {
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = c.audioRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    c.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    c.maxQueue = c.audioRate / 10 * 4; // ~100ms of slack
    SDL_PauseAudioDevice(c.audio, 0);
  }
  SDL_ClearQueuedAudio(c.audio);

  r.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
  memset(corePad, 0, sizeof(corePad));
  coreStick[0] = coreStick[1] = coreCStick[0] = coreCStick[1] = 0;
  c.frameW = c.frameH = 0;
  c.gameLoaded = true;
  return true;
}

void coreUnloadGame(void) {
  if(!c.gameLoaded) return;
  c.gameLoaded = false;
  // The core's context has to be current for this, not just for context_destroy:
  // GLideN64 writes its shader cache out during unload, and against the wrong context
  // its GL queries come back as garbage -- it then happily writes a multi-gigabyte
  // cache file, which the next launch of that game tries to load back in.
  if(c.coreCtx != NULL) SDL_GL_MakeCurrent(c.window, c.coreCtx);
  r.unload_game(); // also flushes the cartridge save to saveDir
  if(c.hwReady) {
    if(c.hw.context_destroy) c.hw.context_destroy();
    if(c.fbo) gl.DeleteFramebuffers(1, &c.fbo);
    if(c.colorTex) glDeleteTextures(1, &c.colorTex);
    if(c.depthRb) gl.DeleteRenderbuffers(1, &c.depthRb);
    c.fbo = c.colorTex = c.depthRb = 0;
    c.hwReady = false;
  }
  if(c.coreCtx != NULL) SDL_GL_MakeCurrent(c.window, c.uiCtx);
  if(c.audio) SDL_ClearQueuedAudio(c.audio);
  c.frameW = c.frameH = 0;
}

bool coreGameLoaded(void) {
  return c.gameLoaded;
}

void coreRunFrame(void) {
  if(!c.gameLoaded) return;
  if(c.coreCtx == NULL) { // software core: nothing to bind, videoCb does the copying
    r.run();
    return;
  }
  SDL_GL_MakeCurrent(c.window, c.coreCtx);
  gl.BindFramebuffer(GL_FRAMEBUFFER, c.fbo);
  r.run(); // calls videoCb, which reads the frame back before we hand the context over
  SDL_GL_MakeCurrent(c.window, c.uiCtx);
}

float coreFrameTime(void) {
  return c.frameTime > 0.0f ? c.frameTime : 1.0f / 60.0f;
}

float coreAspect(void) {
  return c.aspect > 0.1f ? c.aspect : 4.0f / 3.0f;
}

const void* corePixels(int* w, int* h) {
  *w = c.frameW;
  *h = c.frameH;
  return c.frameW > 0 ? c.pixels : NULL;
}

bool coreFlipped(void) {
  return c.hw.bottom_left_origin && !c.software;
}

// what corePixels() rows look like, ready for SDL_CreateTexture
Uint32 corePixelFormat(void) {
  if(!c.software) return SDL_PIXELFORMAT_ABGR8888; // gl gives us rgba bytes
  if(c.swFormat == RETRO_PIXEL_FORMAT_RGB565) return SDL_PIXELFORMAT_RGB565;
  if(c.swFormat == RETRO_PIXEL_FORMAT_0RGB1555) return SDL_PIXELFORMAT_ARGB1555; // the older cores
  return SDL_PIXELFORMAT_ARGB8888;
}

int coreStateSize(void) {
  return c.gameLoaded ? (int) r.serialize_size() : 0;
}

bool coreSaveState(void* buf, int size) {
  return c.gameLoaded && r.serialize(buf, (size_t) size);
}

bool coreLoadState(const void* buf, int size) {
  return c.gameLoaded && r.unserialize(buf, (size_t) size);
}

void coreReset(void) {
  if(c.gameLoaded) r.reset();
}

void coreClose(void) {
  coreUnloadGame();
  if(c.lib != NULL) {
    r.deinit();
    SDL_UnloadObject(c.lib);
    c.lib = NULL;
  }
  if(c.audio) {
    SDL_CloseAudioDevice(c.audio);
    c.audio = 0;
  }
  if(c.coreCtx) {
    SDL_GL_DeleteContext(c.coreCtx);
    c.coreCtx = NULL;
  }
  free(c.pixels);
  c.pixels = NULL;
}
