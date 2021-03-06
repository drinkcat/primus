#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <map>
#include <string>
#include <X11/Xlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#pragma GCC visibility push(default)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#pragma GCC visibility pop

#define primus_print(c, ...) do { if (c) fprintf(stderr, "primus: " __VA_ARGS__); } while (0)

#define die_if(cond, ...)  do {if (cond) {primus_print(true, "fatal: " __VA_ARGS__); exit(1);} } while (0)
#define primus_warn(...) primus_print(primus.loglevel >= 1, "warning: " __VA_ARGS__)
#define primus_perf(...) primus_print(primus.loglevel >= 2, "profiling: " __VA_ARGS__)

// Try to load any of the colon-separated libraries
static void *mdlopen(const char *paths, int flag)
{
  char *p = strdupa(paths);
  char errors[1024], *errors_ptr = errors, *errors_end = errors + 1024;
  for (char *c = p; c; p = c + 1)
  {
    if ((c = strchr(p, ':')))
      *c = 0;
    die_if(p[0] != '/', "need absolute library path: %s\n", p);
    void *handle = dlopen(p, flag);
    if (handle)
      return handle;
    errors_ptr += snprintf(errors_ptr, errors_end - errors_ptr, "%s\n", dlerror());
  }
  die_if(true, "failed to load any of the libraries: %s\n%s", paths, errors);
}

static void *real_dlsym(void *handle, const char *symbol)
{
  typedef void* (*dlsym_fn)(void *, const char*);
  static dlsym_fn pdlsym = (dlsym_fn) dlsym(dlopen("libdl.so.2", RTLD_LAZY), "dlsym");
  return pdlsym(handle, symbol);
}

// Pointers to implemented/forwarded GLX and OpenGL functions
struct CapturedFns {
  void *handle[2]; /* 0: EGL, 1: GLESv2 */

  /* First try to load the symbol with eglGetProcAddress, then fallback on dlsym */
  inline void *egldlsym(const char *symbol)
  {
    void* p = (void*)this->eglGetProcAddress(symbol);
    if (p)
      return p;
    return real_dlsym(handle[1], symbol);
  }

  // Declare functions as fields of the struct
#define DEF_EGL_PROTO(ret, name, args, ...) ret (*name) args;
#include "egl-reimpl.def"
#include "egl-passthru.def"
#include "gles-passthru.def"
#undef DEF_EGL_PROTO
  CapturedFns(const char *libegl, const char *libglesv2)
  {
    handle[0] = mdlopen(libegl, RTLD_LAZY);
    handle[1] = mdlopen(libglesv2, RTLD_LAZY);
    die_if(!handle[0], "Cannot load %s\n", libegl);
    die_if(!handle[1], "Cannot load %s\n", libglesv2);
#define DEF_EGL_PROTO(ret, name, args, ...) do { \
name = (ret (*) args)real_dlsym(handle[0], #name); \
  } while (0);
#include "egl-reimpl.def"
#undef DEF_EGL_PROTO
#define DEF_EGL_PROTO(ret, name, args, ...) do { \
name = (ret (*) args)egldlsym(#name); \
  } while (0);
#include "gles-passthru.def"
#undef DEF_EGL_PROTO
  }
  ~CapturedFns()
  {
    dlclose(handle[0]);
    dlclose(handle[1]);
  }
};

// Drawable tracking info
struct DrawableInfo {
  // Only XWindow is not explicitely created via GLX
  enum {XWindow, Window, Pixmap, Pbuffer} kind;
  EGLConfig config;
  EGLSurface  pbuffer;
  EGLContext maincontext; /* Context in main thread */
  Drawable window;
  int width, height;
  enum ReinitTodo {NONE, RESIZE, SHUTDOWN} reinit;
  void *pixeldata;
  int pixelformat;
  int pixeltype;
  GLsync sync;
  EGLContext actx;

  struct {
    pthread_t worker;
    sem_t acqsem, relsem;
    ReinitTodo reinit;

    void spawn_worker(EGLSurface draw, void* (*work)(void*))
    {
      reinit = RESIZE;
      sem_init(&acqsem, 0, 0);
      sem_init(&relsem, 0, 0);
      pthread_create(&worker, NULL, work, (void*)draw);
    }
    void reap_worker()
    {
      //pthread_cancel(worker);
      pthread_join(worker, NULL);
      sem_destroy(&relsem);
      sem_destroy(&acqsem);
      worker = 0;
    }
  } r, d;
  void reap_workers()
  {
    if (r.worker)
    {
      r.reinit = SHUTDOWN;
      sem_post(&r.acqsem);
      sem_wait(&r.relsem);
      r.reap_worker();
      d.reap_worker();
    }
  }
  void update_geometry(int width, int height)
  {
    if (this->width == width && this->height == height)
      return;
    this->width = width; this->height = height;
    __sync_synchronize();
    reinit = RESIZE;
  }
  ~DrawableInfo();
};

struct DrawablesInfo: public std::map<EGLSurface, DrawableInfo> {
  bool known(EGLSurface draw)
  {
    return this->find(draw) != this->end();
  }
};

struct ContextInfo {
  EGLConfig config;
  int sharegroup;
};

struct ContextsInfo: public std::map<EGLContext, ContextInfo> {
  void record(EGLContext ctx, EGLConfig config, EGLContext share)
  {
    static int nsharegroups;
    int sharegroup = share ? (*this)[share].sharegroup : nsharegroups++;
    (*this)[ctx] = (ContextInfo){config, sharegroup};
  }
};

// Shorthand for obtaining compile-time configurable value that can be
// overridden by environment
#define getconf(V) (getenv(#V) ? getenv(#V) : V)

// Process-wide data
static struct PrimusInfo {
  const char *adpy_str, *libegla_str, *libglesv2a_str;
  // Readback-display synchronization method
  // 0: no sync, 1: D lags behind one frame, 2: fully synced
  int sync;
  // 0: only errors, 1: warnings, 2: profiling
  int loglevel;
  // 0: autodetect, 1: texture, 2: PBO glDrawPixels
  int dispmethod;
  // sleep ratio in readback thread, percent
  int autosleep;
  // The "accelerating" X display
  Display *adpy;
  EGLDisplay adisplay;
  // The "displaying" X display. The same as the application is using, but
  // primus opens its own connection.
  Display *ddpy;
  // An artifact: primus needs to make symbols from libglapi.so globally
  // visible before loading Mesa
  const void *needed_global;
  CapturedFns afns;
  // FIXME: there are race conditions in accesses to these
  DrawablesInfo drawables;
  ContextsInfo contexts;

  /* Fake pointer to return as EGLSurface in createWindow, incremented
   * to make sure it stays unique */
  long fakesurface;

  PrimusInfo():
    adpy_str(getconf(PRIMUS_DISPLAY)),
    libegla_str(getconf(PRIMUS_libEGLa)),
    libglesv2a_str(getconf(PRIMUS_libGLESv2a)),
    sync(atoi(getconf(PRIMUS_SYNC))),
    loglevel(atoi(getconf(PRIMUS_VERBOSE))),
    dispmethod(atoi(getconf(PRIMUS_UPLOAD))),
    autosleep(atoi(getconf(PRIMUS_SLEEP))),
    adpy(XOpenDisplay(adpy_str)),
    ddpy(XOpenDisplay(NULL)),
    needed_global(dlopen(getconf(PRIMUS_LOAD_GLOBAL), RTLD_LAZY | RTLD_GLOBAL)),
    afns(libegla_str, libglesv2a_str),
    fakesurface(0xDEAD0000)
  {
    die_if(!adpy, "failed to open secondary X display\n");
    die_if(!ddpy, "failed to open main X display\n");
    die_if(!needed_global, "failed to load PRIMUS_LOAD_GLOBAL\n");
    XInitThreads();
    loglevel = 2;

    adisplay = afns.eglGetDisplay((EGLNativeDisplayType)adpy);
  }
} primus;

// Thread-specific data
static __thread struct {
  Display *dpy;
  EGLSurface drawable, read_drawable;
  void make_current(Display *dpy, EGLSurface draw, EGLSurface read)
  {
    this->dpy = dpy;
    this->drawable = draw;
    this->read_drawable = read;
  }
} tsdata;

// Profiler
struct Profiler {
  const char *name;
  const char * const *state_names;

  double state_time[6], prev_timestamp, print_timestamp;
  int state, nframes, width, height;

  static double get_timestamp()
  {
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec + 1e-9 * tp.tv_nsec;
  }

  Profiler(const char *name, const char * const *state_names):
    name(name),
    state_names(state_names),
    state(0), nframes(0), width(0), height(0)
  {
    memset(state_time, 0, sizeof(state_time));
    prev_timestamp = print_timestamp = get_timestamp();
  }

  void tick(bool state_reset = false)
  {
    if (primus.loglevel < 2)
      return;
    double timestamp = get_timestamp();
    assert(state_reset || state_names[state]);
    if (state_reset)
      state = 0;
    assert(state * sizeof(state_time[0]) < sizeof(state_time));
    state_time[state++] += timestamp - prev_timestamp;
    prev_timestamp = timestamp;
    if (state_names[state])
      return;
    nframes++;
    // check if it's time to print again
    double period = timestamp - print_timestamp; // time since we printed
    if (period < 5)
      return;
    // construct output
    char buf[128], *cbuf = buf, *end = buf+128;
    for (int i = 0; i < state; i++)
      cbuf += snprintf(cbuf, end - cbuf, ", %.1f%% %s", 100 * state_time[i] / period, state_names[i]);
    primus_perf("%s: %dx%d, %.1f fps%s\n", name, width, height, nframes / period, buf);
    // start counting again
    print_timestamp = timestamp;
    nframes = 0;
    memset(state_time, 0, sizeof(state_time));
  }
};

// Find out the dimensions of the window
static void note_geometry(Display *dpy, Drawable draw, int *width, int *height)
{
  Window root;
  int x, y;
  unsigned bw, d;
  XGetGeometry(dpy, draw, &root, &x, &y, (unsigned *)width, (unsigned *)height, &bw, &d);
}

#if 0
// If atod is true, match adisplay config to ddisplay config. Otherwise
// reverse the operation.
static EGLBoolean match_config(int atod, EGLConfig config, EGLConfig* pconfig)
{
  int ncfg;
  EGLBoolean ret;
  EGLint attrs[] =
  {
    EGL_RED_SIZE,       0,
    EGL_GREEN_SIZE,     0,
    EGL_BLUE_SIZE,      0,
    EGL_ALPHA_SIZE,     0,
    EGL_DEPTH_SIZE,     0,
    EGL_STENCIL_SIZE,   0,
    EGL_SAMPLE_BUFFERS, 0,
    EGL_NONE
  };
  if (atod) {
    for (int i = 0; attrs[i] != EGL_NONE; i += 2)
      primus.afns.eglGetConfigAttrib(primus.adisplay, config, attrs[i], &attrs[i+1]);
    ret = primus.dfns.eglChooseConfig(primus.ddisplay, attrs, pconfig, 1, &ncfg);
  } else {
    for (int i = 0; attrs[i] != EGL_NONE; i += 2)
      primus.dfns.eglGetConfigAttrib(primus.ddisplay, config, attrs[i], &attrs[i+1]);
    ret = primus.afns.eglChooseConfig(primus.adisplay, attrs, pconfig, 1, &ncfg);
  }
  die_if(!ret, "Cannot match config\n");
  return ret;
}
#endif

static void rgb_to_bgra(void* __restrict__ inp, void* __restrict__ outp,
                         int width, int height) {
  die_if(!inp || !outp);
  int i,j;
  uint32_t* in = (uint32_t*)inp;
  uint32_t* out = (uint32_t*)outp;
  int wblock = width/4;
  int wreminder = width%4;

  out += width*(height-1);
  for (j = 0; j < height; j++) {
    uint32_t i0, i1, i2;
    for (i = 0; i < wblock; i++) {
      i0 = in[0];
      i1 = in[1];
      i2 = in[2];
      out[0] = (i0 & 0x000000ff) << 16 | (i0 & 0x0000ff00)       | (i0 & 0x00ff0000) >> 16;
      out[1] = (i0 & 0xff000000) >> 8  | (i1 & 0x000000ff) << 8  | (i1 & 0x0000ff00) >> 8;
      out[2] = (i1 & 0x00ff0000)       | (i1 & 0xff000000) >> 16 | (i2 & 0x000000ff);
      out[3] = (i2 & 0x0000ff00) << 8  | (i2 & 0x00ff0000) >> 8  | (i2 & 0xff000000) >> 24;
      in += 3;
      out += 4;
    }
    if (wreminder > 0) {
      i0 = *in++;
      *out++ = (i0 & 0x000000ff) << 16 | (i0 & 0x0000ff00)       | (i0 & 0x00ff0000) >> 16;
      if (wreminder > 1) {
        i1 = *in++;
          *out++ = (i0 & 0xff000000) >> 8  | (i1 & 0x000000ff) << 8  | (i1 & 0x0000ff00) >> 8;
          if (wreminder > 2) {
            i2 = *in++;
            *out++ = (i1 & 0x00ff0000)       | (i1 & 0xff000000) >> 16 | (i2 & 0x000000ff);
          }
      }
    }
    out -= 2*width;
  }
}

static void rgba_to_bgra(void* __restrict__ inp, void* __restrict__ outp,
                         int width, int height) {
    die_if(!inp || !outp);
    int i,j;
    uint32_t* in = (uint32_t*)inp;
    uint32_t* out = (uint32_t*)outp;

    out += width*(height-1);
    for (j = 0; j < height; j++) {
      for (i = 0; i < width; i ++) {
        // Swap R and B
        int val = *(in++);
        val = ((val >> 16) & 0xff) | ((val & 0xff) << 16) | (val & 0xff00ff00);
        *(out++) = val;
      }
      out -= 2*width;
    }
}

static void bgra_to_bgra(void* __restrict__ inp, void* __restrict__ outp,
                         int width, int height) {
    die_if(!inp || !outp);
    int j;
    uint32_t* in = (uint32_t*)inp;
    uint32_t* out = (uint32_t*)outp;

    out += width*(height-1);
    for (j = 0; j < height; j++) {
      memcpy(out, in, width*4);
      in += width;
      out -= width;
    }
}

static void* display_work(void *vd)
{
  printf("primus display_work\n");
  EGLSurface drawable = (EGLSurface)vd;
  DrawableInfo &di = primus.drawables[drawable];
  int width, height;
  static const char *state_names[] = {"wait", "upload", "draw", "swap", NULL};
  Profiler profiler("display", state_names);
  Display * ddpy = primus.ddpy;
  assert(di.kind == di.XWindow || di.kind == di.Window);
  note_geometry(ddpy, di.window, &width, &height);
  di.update_geometry(width, height);

  GC gc = XCreateGC(ddpy, di.window, 0, NULL);

  char* imgbuffer = NULL;
  XImage* imgnative = NULL;
  XShmSegmentInfo shminfo;

  /* TODO: Fallback if MIT-SHM not available */
  die_if(!XShmQueryExtension(ddpy), "MIT-SHM extension not available.");

  for (;;)
  {
    sem_wait(&di.d.acqsem);
    profiler.tick(true);
    if (di.d.reinit)
    {
      if (imgnative)
        XDestroyImage(imgnative);
      if (di.d.reinit == di.SHUTDOWN)
      {
	XCloseDisplay(ddpy);
	sem_post(&di.d.relsem);
	return NULL;
      }
      di.d.reinit = di.NONE;
      profiler.width = width = di.width;
      profiler.height = height = di.height;
//      imgbuffer = (char*)malloc(width*height*4);
//      imgnative = XCreateImage(ddpy, CopyFromParent, 24, ZPixmap, 0,
//                                     imgbuffer, width, height, 32, 0);
      imgnative = XShmCreateImage(ddpy, CopyFromParent, 24, ZPixmap,
                                  imgbuffer, &shminfo, width, height);
      printf("imgnative=%p\n", imgnative);
      shminfo.shmid = shmget(IPC_PRIVATE, 4*width*height, IPC_CREAT|0777);
      imgbuffer = imgnative->data = shminfo.shmaddr = (char*)shmat(shminfo.shmid, 0, 0);
      shminfo.readOnly = True;
      die_if(!XShmAttach(ddpy, &shminfo), "SHM attach error");

      sem_post(&di.d.relsem);
      continue;
    }

    if (di.pixeltype == GL_UNSIGNED_BYTE) {
      if (di.pixelformat == GL_RGBA)
        rgba_to_bgra(di.pixeldata, imgbuffer, width, height);
      else if (di.pixelformat == GL_RGB)
        rgb_to_bgra(di.pixeldata, imgbuffer, width, height);
      else if (di.pixelformat == GL_BGRA_EXT)
        bgra_to_bgra(di.pixeldata, imgbuffer, width, height);
      else
        die_if(1, "Invalid pixel format.\n");
    } else {
      die_if(1, "Invalid pixel type.\n");
    }

    if (!primus.sync)
      sem_post(&di.d.relsem); // Unlock as soon as possible
    profiler.tick();

    //XPutImage(ddpy, di.window, gc, imgnative, 0, 0, 0, 0, width, height);
    /* FIXME: SendEvent? */
    XShmPutImage(ddpy, di.window, gc, imgnative, 0, 0, 0, 0, width, height, False);

    profiler.tick();
    /* Listening to events does not work (we compete against the application!):
     * just check for window size after each frame. */
    int nwidth, nheight;
    note_geometry(ddpy, di.window, &nwidth, &nheight);
    di.update_geometry(nwidth, nheight);
    if (primus.sync)
      sem_post(&di.d.relsem); // Unlock only after drawing
    profiler.tick();
  }
  return NULL;
}

static void* readback_work(void *vd)
{
  printf("primus readback_work\n");
  /* FIXME: Detect EGL version automatically */
  EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };
  EGLSurface drawable = (EGLSurface)vd;
  DrawableInfo &di = primus.drawables[drawable];
  int width = 0, height = 0;
  int cbuf = 0;
  void* buffers[2] = {NULL, NULL};
  static const char *state_names[] = {"app", "sleep", "readpix", "wait", NULL};
  Profiler profiler("readback", state_names);
  struct timespec tp;
  if (!primus.sync)
    sem_post(&di.d.relsem); // No PBO is mapped initially

  EGLBoolean ret;

  printf("di.pbuffer=%p di.config=%p\n", di.pbuffer, di.config);
  printf("di.maincontext=%p\n", di.maincontext);

  EGLContext context = primus.afns.eglCreateContext(primus.adisplay, di.config, di.maincontext, contextAttribs);
  die_if(!context,
         "failed to acquire rendering context for readback thread\n");
  ret = primus.afns.eglMakeCurrent(primus.adisplay, di.pbuffer, di.pbuffer, context);
  die_if(!ret, "eglMakeCurrent failed in readback thread\n");

  /* FIXME: Does that make any sense? */
  //primus.afns.glReadBuffer(GL_FRONT);

  ret = primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);

  printf("readback_work 2.0\n");
  for (;;)
  {
    sem_wait(&di.r.acqsem);
    profiler.tick(true); 
    /* sleep */
    ret = primus.afns.eglMakeCurrent(primus.adisplay, di.pbuffer, di.pbuffer, context);
    die_if(!ret, "eglMakeCurrent failed in readback thread loop\n");
    if (di.r.reinit)
    {
      printf("readback_reinit\n");
      clock_gettime(CLOCK_REALTIME, &tp);
      tp.tv_sec  += 1;
      // Wait for D worker, if active
      if (!primus.sync && sem_timedwait(&di.d.relsem, &tp))
      {
	pthread_cancel(di.d.worker);
	sem_post(&di.d.relsem); // Pretend that D worker completed reinit
	primus_warn("timeout waiting for display worker\n");
	die_if(di.r.reinit != di.SHUTDOWN, "killed worker on resize\n");
      }
      di.d.reinit = di.r.reinit;
      sem_post(&di.d.acqsem); // Signal D worker to reinit
      sem_wait(&di.d.relsem); // Wait until reinit was completed
      if (!primus.sync)
	sem_post(&di.d.relsem); // Unlock as no PBO is currently mapped

      free(buffers[0]);
      free(buffers[1]);

      if (di.r.reinit == di.SHUTDOWN)
      {
	primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);
	primus.afns.eglDestroyContext(primus.adisplay, context);
	sem_post(&di.r.relsem);
	return NULL;
      }
      di.r.reinit = di.NONE;
      profiler.width = width = di.width;
      profiler.height = height = di.height;
      printf("readback_reinit width=%d height=%d cbuf=%d\n", width, height, cbuf);
      ret = primus.afns.eglMakeCurrent(primus.adisplay, di.pbuffer, di.pbuffer, context);
      die_if(!ret, "eglMakeCurrent failed in readback thread loop\n");
      buffers[0] = malloc(4*width*height);
      buffers[1] = malloc(4*width*height);

      GLint ext_format, ext_type;
      glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &ext_format);
      glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &ext_type);
      printf("FORMAT/TYPE: %x %x\n", ext_format, ext_type);
      printf("FORMAT/TYPE: GL_RGB=%x GL_RGBA=%x\n", GL_RGB, GL_RGBA);
      printf("FORMAT/TYPE: GL_UNSIGNED_BYTE=%x\n", GL_UNSIGNED_BYTE);

      di.pixelformat = ext_format;
      di.pixeltype = ext_type;
    }

    primus.afns.glWaitSync(di.sync, 0, GL_TIMEOUT_IGNORED);

    profiler.tick();
    /* readpix */
    primus.afns.glReadPixels(0, 0, width, height, di.pixelformat, di.pixeltype, buffers[cbuf]);
    GLenum error = glGetError();
    die_if(error, "glReadPixels error: %x\n", error);
    if (!primus.sync) {
      primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);
      sem_post(&di.r.relsem); // Unblock main thread as soon as possible
    }
    if (primus.sync == 1) // Get the previous framebuffer
      di.pixeldata = buffers[cbuf];
    else
      di.pixeldata = buffers[cbuf^1];
    profiler.tick();
    /* wait */
    clock_gettime(CLOCK_REALTIME, &tp);
    tp.tv_sec  += 1;

    if (!primus.sync && sem_timedwait(&di.d.relsem, &tp))
      primus_warn("dropping a frame to avoid deadlock\n");
    else
    {
      sem_post(&di.d.acqsem);
      if (primus.sync)
      {
	sem_wait(&di.d.relsem);
        primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);
	sem_post(&di.r.relsem); // Unblock main thread only after D::work has completed
      }
      cbuf ^= 1;
    }
    profiler.tick();
  }
  return NULL;
}

EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share_context,
                            EGLint const *attrib_list)
{
  printf("primus eglCreateContext\n");
  EGLContext actx = primus.afns.eglCreateContext(primus.adisplay, config, share_context, attrib_list);
  die_if ( actx == EGL_NO_CONTEXT, "eglCreateContext failed.\n");
  printf("primus eglCreateContext config=%p config=%p\n", config, config);
  primus.contexts.record(actx, config, share_context);
  return actx;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
  primus.contexts.erase(ctx);
  // kludge: reap background tasks when deleting the last context
  // otherwise something will deadlock during unloading the library
  if (primus.contexts.empty())
    for (DrawablesInfo::iterator i = primus.drawables.begin(); i != primus.drawables.end(); i++)
      i->second.reap_workers();
  return primus.afns.eglDestroyContext(primus.adisplay, ctx);
}

static EGLSurface create_pbuffer(EGLDisplay dpy, DrawableInfo &di)
{
  /* Pixel buffer attributes. */
  EGLint pbattrs[] = {
    EGL_WIDTH, di.width,
    EGL_HEIGHT, di.height,
    EGL_NONE
  };
  EGLSurface surface = primus.afns.eglCreatePbufferSurface(dpy, di.config, pbattrs);
  printf("create_pbuffer %p %d %d config=%p %p\n", dpy, di.width, di.height, di.config, surface);

  return surface;
}

// Create or recall backing Pbuffer for the drawable
static EGLSurface lookup_pbuffer(EGLDisplay dpy, EGLSurface draw, EGLContext ctx)
{
  if (!draw)
    return 0;
  bool known = primus.drawables.known(draw);
  DrawableInfo &di = primus.drawables[draw];
  if (!known)
  {
    // Drawable is a plain X Window. Get the Config from the context
    if (ctx) {
      di.config = primus.contexts[ctx].config;
    }
    else
    {
/* FIXME: Find window from draw? */
#if 0
      XWindowAttributes attrs;
      die_if(!XGetWindowAttributes(dpy, draw, &attrs), "failed to query attributes");
      int nvis;
      XVisualInfo tmpl = {0}, *vis;
      tmpl.visualid = XVisualIDFromVisual(attrs.visual);
      die_if(!(vis = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &nvis)), "no visuals");
      di.config = *match_config(vis);
      XFree(vis);
#endif
    }
    di.kind = di.XWindow;
    note_geometry(primus.ddpy, di.window, &di.width, &di.height);
  }
  else if (ctx && di.config != primus.contexts[ctx].config)
  {
    if (di.pbuffer)
    {
      primus_warn("recreating incompatible pbuffer\n");
      di.reap_workers();
      primus.afns.eglDestroySurface(primus.adisplay, di.pbuffer);
      di.pbuffer = 0;
    }
    di.config = primus.contexts[ctx].config;
  }
  if (!di.pbuffer) {
    di.pbuffer = create_pbuffer(dpy, di);
    di.maincontext = ctx;
  }
  return di.pbuffer;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
  printf("primus eglMakeCurrent %p, %p, %p, %p\n", dpy, draw, read, ctx);
  EGLSurface pbuffer = lookup_pbuffer(dpy, draw, ctx);
  printf("pbuffer=%p\n", pbuffer);
  tsdata.make_current(primus.ddpy, draw, read); //FIXME: check dpy 
  EGLBoolean ret = primus.afns.eglMakeCurrent(primus.adisplay, pbuffer, pbuffer, ctx);
  printf("ret=%d\n", ret);
  return ret;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface drawable)
{
  XFlush(primus.ddpy);
  assert(primus.drawables.known(drawable));
  DrawableInfo &di = primus.drawables[drawable];
  if (!primus.afns.eglSwapBuffers(dpy, di.pbuffer)) {
    printf("primus swap error.\n");
    exit(0);
    return 0;
  }
  if (di.kind == di.Pbuffer || di.kind == di.Pixmap)
    return 1;
  EGLContext ctx = primus.afns.eglGetCurrentContext();
  if (!ctx)
    primus_warn("glXSwapBuffers: no current context\n");
  else if (drawable != tsdata.drawable)
    primus_warn("glXSwapBuffers: drawable not current\n");
  if (di.r.worker && ctx && (!di.actx || primus.contexts[di.actx].sharegroup != primus.contexts[ctx].sharegroup))
  {
    primus_warn("glXSwapBuffers: respawning threads after context change\n");
    di.reap_workers();
  }
  // Readback thread needs a sync object to avoid reading an incomplete frame
  di.sync = primus.afns.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  EGLBoolean ret = primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);
  //printf("swap: eglMakeCurrent release ret=%d\n", ret);
  if (!di.r.worker)
  {
    // Need to create a sharing context to use GL sync objects
    di.actx = ctx;
    di.d.spawn_worker(drawable, display_work);
    di.r.spawn_worker(drawable, readback_work);
  }
  sem_post(&di.r.acqsem); // Signal the readback worker thread
  sem_wait(&di.r.relsem); // Wait until it has issued glReadBuffer
  //printf("eglMakeCurrent %p %p %p %p\n", primus.adisplay, tsdata.drawable, tsdata.read_drawable, ctx);
  //ret = eglMakeCurrent(dpy, tsdata.drawable, tsdata.read_drawable, ctx);
  primus.afns.eglMakeCurrent(primus.adisplay, di.pbuffer, di.pbuffer, ctx);
  die_if(!ret, "Failed eglMakeCurrent\n");
  //printf("Delete sync\n");
  primus.afns.glDeleteSync(di.sync);
  //printf("Done");
  if (di.reinit == di.RESIZE)
  {
    __sync_synchronize();
    printf("Resize\n");
    primus.afns.eglDestroySurface(primus.adisplay, di.pbuffer);
    di.pbuffer = create_pbuffer(primus.adisplay, di);
    if (ctx) // FIXME: drawable can be current in other threads
      eglMakeCurrent(dpy, tsdata.drawable, tsdata.read_drawable, ctx);
    di.r.reinit = di.reinit;
    di.reinit = di.NONE;
  }
  return 1;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
  printf("primus eglGetDisplay\n");
  primus.ddpy = display_id;

  return primus.adisplay;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType win, EGLint const* attribList) {
  EGLSurface surface = (EGLSurface)primus.fakesurface++;
  DrawableInfo &di = primus.drawables[surface];
  di.kind = di.Window;
  di.config = config;
  di.window = win;
  note_geometry(primus.ddpy, win, &di.width, &di.height);
  return surface;
}

DrawableInfo::~DrawableInfo()
{
  reap_workers();
  if (pbuffer)
    primus.afns.eglDestroySurface(primus.adisplay, pbuffer);
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
  assert(primus.drawables.known(surface));
  primus.drawables.erase(surface);
  return EGL_TRUE;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attribList)
{
  /* FIXME! */
  EGLSurface pbuffer = (EGLSurface)primus.fakesurface++;
  DrawableInfo &di = primus.drawables[pbuffer];
  di.kind = di.Pbuffer;
  di.config = config;
  for (int i = 0; attribList[i] != None; i++)
    if (attribList[i] == EGL_WIDTH)
      di.width = attribList[i+1];
    else if (attribList[i] == EGL_HEIGHT)
      di.height = attribList[i+1];
  return pbuffer;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
  printf("eglGetConfigAttrib\n");
  if (attribute == EGL_NATIVE_VISUAL_ID && *value) {
    printf("Getting visual ID.\n");
    /* FIXME: Is this acceptable? */
    Visual* vis = DefaultVisual(primus.ddpy, 0);
    *value = (EGLint)XVisualIDFromVisual(vis);
    return EGL_TRUE;
  } else {
    return primus.afns.eglGetConfigAttrib(primus.adisplay, config, attribute, value);
  }
}

EGLContext eglGetCurrentContext(void)
{
  return primus.afns.eglGetCurrentContext();
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface draw, EGLint attribute, EGLint *value)
{
  return primus.afns.eglQuerySurface(primus.adpy, lookup_pbuffer(dpy, draw, NULL), attribute, value);
}

/* FIXME: add eglGetCurrentSurface */

// OpenGL ES forwarders
#define DEF_EGL_PROTO(ret, name, par, ...) \
static ret l##name par \
{ return primus.afns.name(__VA_ARGS__); } \
asm(".type " #name ", %gnu_indirect_function"); \
void *ifunc_##name(void) asm(#name) __attribute__((visibility("default"))); \
void *ifunc_##name(void) \
{ \
  void* val = primus.afns.handle[1] ? primus.afns.egldlsym(#name) : (void*)l##name; \
  return val;                                                         \
}
#include "gles-passthru.def"
#undef DEF_EGL_PROTO

// EGL forwarders
#define DEF_EGL_PROTO(ret, name, par, ...) \
static ret l##name par \
{ return primus.afns.name(__VA_ARGS__); } \
asm(".type " #name ", %gnu_indirect_function"); \
void *ifunc_##name(void) asm(#name) __attribute__((visibility("default"))); \
void *ifunc_##name(void) \
{ \
  void* val = primus.afns.handle[0] ? primus.afns.egldlsym(#name) : (void*)l##name; \
  return val;                                                         \
}
#include "egl-passthru.def"
#undef DEF_EGL_PROTO

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procName)
{
    printf("eglGetProcAddress: %s\n", procName);
  static const char * const redefined_names[] = {
#define DEF_EGL_PROTO(ret, name, args, ...) #name,
#include "egl-reimpl.def"
#undef  DEF_EGL_PROTO
  };
  static const __eglMustCastToProperFunctionPointerType redefined_fns[] = {
#define DEF_EGL_PROTO(ret, name, args, ...) (__eglMustCastToProperFunctionPointerType)name,
#include "egl-reimpl.def"
#undef  DEF_EGL_PROTO
  };
  enum {n_redefined = sizeof(redefined_fns) / sizeof(redefined_fns[0])};
  // Non-GLX functions are forwarded to the accelerating libGL
  if (memcmp(procName, "egl", 3))
    return primus.afns.eglGetProcAddress(procName);
  // All GLX functions are either implemented in primus or not available
  for (int i = 0; i < n_redefined; i++)
    if (!strcmp((const char *)procName, redefined_names[i]))
      return redefined_fns[i];
  return NULL;
}

#if 0
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *procName)
{
  return glXGetProcAddress(procName);
}

static const char glxext_clientside[] = "GLX_ARB_get_proc_address ";
static const char glxext_adpy[] = "GLX_ARB_create_context GLX_ARB_create_context_profile ";
static const char glxext_ddpy[] = "";

const char *glXGetClientString(Display *dpy, int name)
{
  static std::string exts(std::string(glxext_clientside) + glxext_adpy + glxext_ddpy);
  switch (name)
  {
    case GLX_VENDOR: return "primus";
    case GLX_VERSION: return "1.4";
    case GLX_EXTENSIONS: return exts.c_str();
    default: return NULL;
  }
}

static std::string intersect_exts(const char *set1, const char *set2)
{
  std::string r;
  for (const char *p; *set1; set1 = p + 1)
  {
    p = strchr(set1, ' ');
    if (memmem(set2, strlen(set2), set1, p - set1))
      r.append(set1, p - set1 + 1);
  }
  return r;
}

const char *glXQueryExtensionsString(Display *dpy, int screen)
{
  static std::string exts
    (std::string(glxext_clientside)
     + intersect_exts(glxext_adpy, primus.afns.glXQueryExtensionsString(primus.adpy, 0))
     + intersect_exts(glxext_ddpy, primus.dfns.glXQueryExtensionsString(primus.ddpy, 0)));
  return exts.c_str();
}
#endif

#ifndef PRIMUS_STRICT
#warning Enabled extra EGL functions pass-thru
#define P(name) \
asm(".type " #name ", %gnu_indirect_function"); \
void *ifunc_##name(void) asm(#name) __attribute__((visibility("default"))); \
void *ifunc_##name(void) \
{ \
  printf("passthru calling %s %p\n", #name, primus.afns.handle[0]);   \
  void* val = primus.afns.handle[0] ? real_dlsym(primus.afns.handle[0], #name) : NULL; \
  printf("passthru val %p\n", val);                                     \
  return val;                                                           \
}
#include "egl-passthru-extra.def"
#undef P
#endif
