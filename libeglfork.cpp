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
#include <X11/Xatom.h>
#pragma GCC visibility push(default)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#pragma GCC visibility pop

#define primus_print(c, ...) do { if (c) fprintf(stderr, "primus: " __VA_ARGS__); } while (0)

#define die_if(cond, ...)  do {if (cond) {primus_print(true, "fatal: " __VA_ARGS__); exit(1);} } while (0)
#define primus_warn(...) primus_print(primus.loglevel >= 1, "warning: " __VA_ARGS__)
#define primus_perf(...) primus_print(primus.loglevel >= 2, "profiling: " __VA_ARGS__)

// Pointers to implemented/forwarded GLX and OpenGL functions
struct CapturedFns {
//  void *handle;

private:
  int handlecount;
  void *handle[16];
  typedef void* (*dlsym_fn)(void *, const char*);
  dlsym_fn pdlsym;

  // Try to load any of the colon-separated libraries
  void mdlopen(const char *paths, int flag)
  {
    char *p = strdupa(paths);
    char errors[1024], *errors_ptr = errors, *errors_end = errors + 1024;
    for (char *c = p; c; p = c + 1)
    {
      if ((c = strchr(p, ':')))
        *c = 0;
      die_if(p[0] != '/', "need absolute library path: %s\n", p);
      void *c_handle = dlopen(p, flag);
      if (c_handle)
      {
        handle[handlecount] = c_handle;
        handlecount++;
        if (handlecount == 16)
          break;
      }
      errors_ptr += snprintf(errors_ptr, errors_end - errors_ptr, "%s\n", dlerror());
    }
    die_if(handlecount == 0, "failed to load any of the libraries: %s\n%s", paths, errors);
  }

public:
  int handle_valid() {
    return handlecount > 0;
  }

  void *dlsym(const char *symbol)
  {
    //printf("Loading %s\n", symbol);
    int i;
    for (i = 0; i < handlecount; i++) {
      void* p = pdlsym(handle[i], symbol);
      if (p)
        return p;
    }
    return NULL;
  }

  /* First try to load the symbol with eglGetProcAddress, then fallback on dlsym */
  void *egldlsym(const char *symbol)
  {
    void* p = (void*)this->eglGetProcAddress(symbol);
    if (p)
      return p;
    return dlsym(symbol);
  }

  // Declare functions as fields of the struct
#define DEF_EGL_PROTO(ret, name, args, ...) ret (*name) args;
#include "egl-reimpl.def"
#include "egl-dpyredir.def"
#include "gles-passthru.def"
#include "gles-needed.def"
#undef DEF_EGL_PROTO
  CapturedFns(const char *lib)
  {
    handlecount = 0;
    pdlsym = (dlsym_fn)::dlsym(dlopen("libdl.so.2", RTLD_LAZY), "dlsym");
    printf("lib=%s\n", lib);
    mdlopen(lib, RTLD_LAZY);
#define DEF_EGL_PROTO(ret, name, args, ...) do { \
name = (ret (*) args)dlsym(#name); \
printf("%s=%p\n", #name, name);                 \
  } while (0);
#include "egl-reimpl.def"
#include "egl-dpyredir.def"
#undef DEF_EGL_PROTO
#define DEF_EGL_PROTO(ret, name, args, ...) do { \
name = (ret (*) args)egldlsym(#name); \
printf("B %s=%p\n", #name, name);                 \
  } while (0);
#include "gles-passthru.def"
#include "gles-needed.def"
#undef DEF_EGL_PROTO
  }
  ~CapturedFns()
  {
    int i;
    for (i = 0; i < handlecount; i++) {
        dlclose(handle[i]);
    }
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

// Runs before all other initialization takes place
struct EarlyInitializer {
  EarlyInitializer(const char **adpy_strp, const char **libgla_strp)
  {
    //FIXME: no-op
  }
};

// Process-wide data
static struct PrimusInfo {
  const char *adpy_str, *libgla_str;
  EarlyInitializer ei;
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
  EGLConfig *dconfigs;

  /* Fake pointer to return as EGLSurface in createWindow, incremented
   * to make sure it stays unique */
  long fakesurface;

  PrimusInfo():
    adpy_str(getconf(PRIMUS_DISPLAY)),
    libgla_str(getconf(PRIMUS_libGLa)),
    ei(&adpy_str, &libgla_str),
    sync(atoi(getconf(PRIMUS_SYNC))),
    loglevel(atoi(getconf(PRIMUS_VERBOSE))),
    dispmethod(atoi(getconf(PRIMUS_UPLOAD))),
    autosleep(atoi(getconf(PRIMUS_SLEEP))),
    adpy(XOpenDisplay(adpy_str)),
    ddpy(XOpenDisplay(NULL)),
    needed_global(dlopen(getconf(PRIMUS_LOAD_GLOBAL), RTLD_LAZY | RTLD_GLOBAL)),
    afns(libgla_str),
    fakesurface(0xDEAD0000)
  {
    die_if(!adpy, "failed to open secondary X display\n");
    die_if(!ddpy, "failed to open main X display\n");
    die_if(!needed_global, "failed to load PRIMUS_LOAD_GLOBAL\n");
    XInitThreads();
    sync = 0;
    loglevel = 2;
    printf("loglevel=%d\n", loglevel);

    EGLint majorVersion;
    EGLint minorVersion;
    EGLBoolean ret;

    printf("PRIMUS INIT\n");

    adisplay = afns.eglGetDisplay((EGLNativeDisplayType)adpy);

    ret = afns.eglInitialize(adisplay, &majorVersion, &minorVersion);
    die_if(!ret, "broken EGL on accel X display (eglInitialize)\n");

    printf("PRIMUS INIT DONE\n");
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
  printf("Geometry %p %lu %d %d\n", dpy, draw, *width, *height);
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

static void* display_work(void *vd)
{
  printf("primus display_work\n");
  EGLSurface drawable = (EGLSurface)vd;
  DrawableInfo &di = primus.drawables[drawable];
  int width, height;
  static const char *state_names[] = {"wait", "upload", "draw", "swap", NULL};
  Profiler profiler("display", state_names);
  //Display *ddpy = XOpenDisplay(NULL);
  Display * ddpy = primus.ddpy;
  assert(di.kind == di.XWindow || di.kind == di.Window);
  XSelectInput(ddpy, di.window, StructureNotifyMask);
  note_geometry(ddpy, di.window, &width, &height);
  di.update_geometry(width, height);

  GC gc = XCreateGC(ddpy, di.window, 0, NULL);

  char* imgbuffer = NULL;
  XImage* imgnative = NULL;

  for (;;)
  {
    sem_wait(&di.d.acqsem);
    profiler.tick(true);
    if (di.d.reinit)
    {
      //FIXME: Memory leak!
      //free(imgbuffer);
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
      imgbuffer = (char*)malloc(width*height*4);
      imgnative = XCreateImage(ddpy, CopyFromParent, 24, ZPixmap, 0,
                                     imgbuffer, width, height, 32, 0);
      printf("imgbuffer=%p\n", imgbuffer);
      sem_post(&di.d.relsem);
      continue;
    }

#if 0 // RGB 24-bit to 32-bit
    /* FIXME: Data needs to be flipped... */
    int i,j;
    int instride = (width*3+3)/4;
    int wblock = width/4;
    int wreminder = width%4;
    uint32_t* in = (uint32_t*)di.pixeldata;
    uint32_t* out = (uint32_t*)imgbuffer;
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
    }
#endif
#if 1 // RGBA 32-bit to 32-bit
    int i,j;
    uint32_t* in = (uint32_t*)di.pixeldata;
    uint32_t* out = (uint32_t*)imgbuffer;

    die_if(!di.pixeldata || !imgbuffer);

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
#endif

    if (!primus.sync)
      sem_post(&di.d.relsem); // Unlock as soon as possible
    profiler.tick();

    XPutImage(ddpy, di.window, gc, imgnative, 0, 0, 0, 0, width, height);
    XFlush(ddpy);

    profiler.tick();
    for (int pending = XPending(ddpy); pending > 0; pending--)
    {
      XEvent event;
      XNextEvent(ddpy, &event);
      if (event.type == ConfigureNotify)
	di.update_geometry(event.xconfigure.width, event.xconfigure.height);
    }
    if (primus.sync)
      sem_post(&di.d.relsem); // Unlock only after drawing
    profiler.tick();
  }
  return NULL;
}

static void* readback_work(void *vd)
{
  printf("primus readback_work\n");
  EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };
  EGLSurface drawable = (EGLSurface)vd;
  DrawableInfo &di = primus.drawables[drawable];
  int width = 0, height = 0;
  int cbuf = 0;
  void* buffers[2] = {NULL, NULL};
  unsigned sleep_usec = 0;
  static const char *state_names[] = {"app", "sleep", "map", "wait", NULL};
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
/*  die_if(!primus.afns.glXIsDirect(primus.adpy, context),
    "failed to acquire direct rendering context for readback thread\n");*/
  ret = primus.afns.eglMakeCurrent(primus.adisplay, di.pbuffer, di.pbuffer, context);
  printf("readback eglMakeCurrent ret=%d\n", ret);
  die_if(!ret, "eglMakeCurrent failed in readback thread\n");
  primus.afns.glReadBuffer(GL_FRONT);

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
      free(buffers[0]);
      free(buffers[1]);
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
    }

    GLint ext_format, ext_type;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &ext_format);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &ext_type);
/*    printf("FORMAT/TYPE: %x %x\n", ext_format, ext_type);
    printf("FORMAT/TYPE: GL_RGB=%x GL_RGBA=%x\n", GL_RGB, GL_RGBA);
    printf("FORMAT/TYPE: GL_UNSIGNED_BYTE=%x\n", GL_UNSIGNED_BYTE);*/
    /* FIXME: Detect if RGB or RGBA need to be used */

    primus.afns.glWaitSync(di.sync, 0, GL_TIMEOUT_IGNORED);

    /* RGBA is painfully slow on MALI (done in CPU?), if the configuration does not
     * have an alpha channel to start with */
    primus.afns.glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffers[cbuf]);
    if (!primus.sync) {
      //sem_post(&di.r.relsem); // Unblock main thread as soon as possible
    }
    usleep(sleep_usec);
    profiler.tick();
    /* map */
    if (primus.sync == 1) // Get the previous framebuffer
      di.pixeldata = buffers[cbuf];
    else
      di.pixeldata = buffers[cbuf^1];
    double map_time = Profiler::get_timestamp();
    //printf("pixeldata=%p\n", pixeldata);
    map_time = Profiler::get_timestamp() - map_time;
    sleep_usec = (map_time * 1e6 + sleep_usec) * primus.autosleep / 100;
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
	//sem_post(&di.r.relsem); // Unblock main thread only after D::work has completed
      }
      cbuf ^= 1;
    }
    //printf("pixeldatasum=%u\n", sum);
    //printf("Releasing egl context in readback\n");
    primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);
    //printf("done\n");
    sem_post(&di.r.relsem); // Unblock main thread only after D::work has completed
    profiler.tick();
  }
  return NULL;
}

EGLBoolean eglBindAPI(EGLenum  api) {
  return primus.afns.eglBindAPI(api);
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

#if 0
  EGLint val;

  primus.afns.eglQuerySurface(dpy, surface,  EGL_CONFIG_ID, &val);
  printf("eglQuerySurface=%d\n", val);
#endif

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
    note_geometry(primus.ddpy, di.window, &di.width, &di.height); //FIXME: check dpy
  }
  else if (ctx && di.config != primus.contexts[ctx].config)
  {
    printf("case 2 %p %p\n", di.config, primus.contexts[ctx].config);
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
  //printf("primus eglSwapBuffers %p %p\n", dpy, drawable);
  XFlush(primus.adpy); //FIXME: check dpy
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

/* FIXME: This removes the need for passthru hacks */
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

#if 0
GLXPixmap glXCreatePixmap(Display *dpy, EGLConfig config, Pixmap pixmap, const int *attribList)
{
  GLXPixmap glxpix = primus.dfns.glXCreatePixmap(dpy, primus.dconfigs[0], pixmap, attribList);
  DrawableInfo &di = primus.drawables[glxpix];
  di.kind = di.Pixmap;
  di.config = config;
  note_geometry(dpy, pixmap, &di.width, &di.height);
  return glxpix;
}

void glXDestroyPixmap(Display *dpy, GLXPixmap pixmap)
{
  assert(primus.drawables.known(pixmap));
  primus.drawables.erase(pixmap);
  primus.dfns.glXDestroyPixmap(dpy, pixmap);
}

GLXPixmap glXCreateGLXPixmap(Display *dpy, XVisualInfo *visual, Pixmap pixmap)
{
  GLXPixmap glxpix = primus.dfns.glXCreateGLXPixmap(dpy, visual, pixmap);
  DrawableInfo &di = primus.drawables[glxpix];
  di.kind = di.Pixmap;
  note_geometry(dpy, pixmap, &di.width, &di.height);
  EGLConfig *acfgs = match_config(visual);
  di.config = *acfgs;
  return glxpix;
}

void glXDestroyGLXPixmap(Display *dpy, GLXPixmap pixmap)
{
  glXDestroyPixmap(dpy, pixmap);
}

static XVisualInfo *match_visual(int attrs[])
{
  XVisualInfo *vis = glXChooseVisual(primus.ddpy, 0, attrs);
  for (int i = 2; attrs[i] != None && vis; i += 2)
  {
    int tmp = attrs[i+1];
    primus.dfns.glXGetConfig(primus.ddpy, vis, attrs[i], &attrs[i+1]);
    if (tmp != attrs[i+1])
      vis = NULL;
  }
  return vis;
}

XVisualInfo *glXGetVisualFromConfig(Display *dpy, EGLConfig config)
{
  if (!primus.afns.glXGetVisualFromConfig(primus.adpy, config))
    return NULL;
  int i, attrs[] = {
    GLX_RGBA, GLX_DOUBLEBUFFER,
    GLX_RED_SIZE, 0, GLX_GREEN_SIZE, 0, GLX_BLUE_SIZE, 0,
    GLX_ALPHA_SIZE, 0, GLX_DEPTH_SIZE, 0, GLX_STENCIL_SIZE, 0,
    GLX_SAMPLE_BUFFERS, 0, GLX_SAMPLES, 0, None
  };
  for (i = 2; attrs[i] != None; i += 2)
    primus.afns.glXGetConfigAttrib(primus.adpy, config, attrs[i], &attrs[i+1]);
  XVisualInfo *vis = NULL;
  for (i -= 2; i >= 0 && !vis; i -= 2)
  {
    vis = match_visual(attrs);
    attrs[i] = None;
  }
  return vis;
}
#endif

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
  printf("eglGetConfigAttrib\n");
  if (attribute == EGL_NATIVE_VISUAL_ID && *value) {
    /* FIXME? */
    printf("Getting visual ID.\n");
    //return primus.dfns.eglGetConfigAttrib(primus.ddisplay, dcfg, attribute, value);
    return primus.afns.eglGetConfigAttrib(primus.adisplay, config, attribute, value);
  } else {
    return primus.afns.eglGetConfigAttrib(primus.adisplay, config, attribute, value);
  }
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface draw, EGLint attribute, EGLint *value)
{
  return primus.afns.eglQuerySurface(primus.adpy, lookup_pbuffer(dpy, draw, NULL), attribute, value);
}

#if 0
void glXUseXFont(Font font, int first, int count, int list)
{
  unsigned long prop;
  XFontStruct *fs = XQueryFont(primus.ddpy, font);
  XGetFontProperty(fs, XA_FONT, &prop);
  char *xlfd = XGetAtomName(primus.ddpy, prop);
  Font afont = XLoadFont(primus.adpy, xlfd);
  primus.afns.glXUseXFont(afont, first, count, list);
  XUnloadFont(primus.adpy, afont);
  XFree(xlfd);
  XFreeFontInfo(NULL, fs, 1);
}
#endif

EGLContext eglGetCurrentContext(void)
{
  return primus.afns.eglGetCurrentContext();
}

#if 0
EGLSurface glXGetCurrentDrawable(void)
{
  return tsdata.drawable;
}

void glXWaitGL(void)
{
}

void glXWaitX(void)
{
}

Display *glXGetCurrentDisplay(void)
{
  return tsdata.dpy;
}

EGLSurface glXGetCurrentReadDrawable(void)
{
  return tsdata.read_drawable;
}

// Application sees ddpy-side Visuals, but adpy-side Configs and Contexts
XVisualInfo* glXChooseVisual(Display *dpy, int screen, int *attribList)
{
  return primus.dfns.glXChooseVisual(dpy, screen, attribList);
}

int glXGetConfig(Display *dpy, XVisualInfo *visual, int attrib, int *value)
{
  return primus.dfns.glXGetConfig(dpy, visual, attrib, value);
}
#endif

// GLX forwarders that reroute to adpy
#define DEF_EGL_PROTO(ret, name, par, ...) \
ret name par \
{ printf("Redirecting %s\n", #name);                                  \
return primus.afns.name(dpy, __VA_ARGS__); }
#include "egl-dpyredir.def"
#undef DEF_EGL_PROTO

// OpenGL forwarders
#define DEF_EGL_PROTO(ret, name, par, ...) \
static ret l##name par \
{ return primus.afns.name(__VA_ARGS__); } \
asm(".type " #name ", %gnu_indirect_function"); \
void *ifunc_##name(void) asm(#name) __attribute__((visibility("default"))); \
void *ifunc_##name(void) \
{ \
    /*printf("OGL Calling %s\n", #name);*/                              \
    void* val = primus.afns.handle_valid() ? primus.afns.dlsym(#name) : (void*)l##name; \
        /*printf("Val %p\n", val);   */                                 \
    return val;\
}
#include "gles-passthru.def"
#undef DEF_EGL_PROTO

// GLX extensions
#if 0
int glXSwapIntervalSGI(int interval)
{
  return 1; // Indicate failure to set swapinterval
}
#endif

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procName)
{
    printf("eglGetProcAddress: %s\n", procName);
  static const char * const redefined_names[] = {
#define DEF_EGL_PROTO(ret, name, args, ...) #name,
#include "egl-reimpl.def"
#include "egl-dpyredir.def"
#undef  DEF_EGL_PROTO
  };
  static const __eglMustCastToProperFunctionPointerType redefined_fns[] = {
#define DEF_EGL_PROTO(ret, name, args, ...) (__eglMustCastToProperFunctionPointerType)name,
#include "egl-reimpl.def"
#include "egl-dpyredir.def"
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

// OpenGL ABI specifies that anything above OpenGL 1.2 + ARB_multitexture must
// be obtained via glXGetProcAddress, but some applications link against
// extension functions, and Mesa and vendor libraries let them
#ifndef PRIMUS_STRICT
#warning Enabled workarounds for applications demanding more than promised by the OpenGL ABI

// OpenGL extension forwarders
#define P(name) \
asm(".type " #name ", %gnu_indirect_function"); \
void *ifunc_##name(void) asm(#name) __attribute__((visibility("default"))); \
void *ifunc_##name(void) \
{ \
  printf("non-strict Calling %s\n", #name);                             \
  void* val = primus.afns.handle_valid() ? primus.afns.dlsym(#name) : NULL; \
  printf("Val %p\n", val);                                              \
  return val;                                                           \
}
#include "egl-passthru.def"
#undef P
#endif
