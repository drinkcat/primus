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
    if (c_handle) {
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
  EGLSurface windowsurface;
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
#ifdef BUMBLEBEE_SOCKET
    // Signal the Bumblebee daemon to bring up secondary X
    errno = 0;
    int sock = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, getconf(BUMBLEBEE_SOCKET), sizeof(addr.sun_path));
    connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    die_if(errno, "failed to connect to Bumblebee daemon: %s\n", strerror(errno));
    static char c[256];
    if (!getenv("PRIMUS_DISPLAY"))
    {
      send(sock, "Q VirtualDisplay", strlen("Q VirtualDisplay") + 1, 0);
      recv(sock, &c, 255, 0);
      die_if(memcmp(c, "Value: ", strlen("Value: ")), "unexpected query response\n");
      *strchrnul(c, '\n') = 0;
      *adpy_strp = strdup(c + 7);
    }
    if (!getenv("PRIMUS_libGLa"))
    {
      send(sock, "Q LibraryPath", strlen("Q LibraryPath") + 1, 0);
      recv(sock, &c, 255, 0);
      die_if(memcmp(c, "Value: ", strlen("Value: ")), "unexpected query response\n");
      *strchrnul(c, '\n') = 0;
      int npaths = 0;
      for (char *p = c + 7; *p; npaths++, p = strchrnul(p + 1, ':'));
      if (npaths)
      {
	char *bblibs = new char[strlen(c + 7) + npaths * strlen("/libGL.so.1") + 1], *b = bblibs, *n, *p;
	for (p = c + 7; *p; p = n)
	{
	  n = strchrnul(p + 1, ':');
	  b += sprintf(b, "%.*s/libGL.so.1", (int)(n - p), p);
	}
	*libgla_strp = bblibs;
      }
    }
    send(sock, "C", 1, 0);
    recv(sock, &c, 255, 0);
    die_if(c[0] == 'N', "Bumblebee daemon reported: %s\n", c + 5);
    die_if(c[0] != 'Y', "failure contacting Bumblebee daemon\n");
    // the socket will be closed when the application quits, then bumblebee will shut down the secondary X
#else
//#warning Building without Bumblebee daemon support
#endif
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
  EGLDisplay ddisplay;
  // An artifact: primus needs to make symbols from libglapi.so globally
  // visible before loading Mesa
  const void *needed_global;
  CapturedFns afns;
  CapturedFns dfns;
  // FIXME: there are race conditions in accesses to these
  DrawablesInfo drawables;
  ContextsInfo contexts;
  EGLConfig *dconfigs;

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
    dfns(getconf(PRIMUS_libGLd))
  {
    die_if(!adpy, "failed to open secondary X display\n");
    die_if(!ddpy, "failed to open main X display\n");
    die_if(!needed_global, "failed to load PRIMUS_LOAD_GLOBAL\n");
    //FIXME: int ncfg, attrs[] = {GLX_DOUBLEBUFFER, GL_TRUE, None};
    int ncfg;
    EGLint attrs[] =
        {
            EGL_RED_SIZE,       8,
            EGL_GREEN_SIZE,     8,
            EGL_BLUE_SIZE,      8,
            EGL_ALPHA_SIZE,     EGL_DONT_CARE,
            EGL_DEPTH_SIZE,     EGL_DONT_CARE,
            EGL_STENCIL_SIZE,   EGL_DONT_CARE,
            EGL_SAMPLE_BUFFERS, 0,
            EGL_NONE
        };

   EGLint majorVersion;
   EGLint minorVersion;
   EGLBoolean ret;

   printf("PRIMUS INIT\n");

    adisplay = afns.eglGetDisplay((EGLNativeDisplayType)adpy);
    ddisplay = dfns.eglGetDisplay((EGLNativeDisplayType)ddpy);

    ret = afns.eglInitialize(adisplay, &majorVersion, &minorVersion);
    die_if(!ret, "broken EGL on accel X display (eglInitialize)\n");
    ret = dfns.eglInitialize(ddisplay, &majorVersion, &minorVersion);
    die_if(!ret, "broken EGL on main X display (eglInitialize)\n");

    /* FIXME: Do we need the list of config? Or just the first one? */
/*    die_if(!eglGetConfigs(ddpy, NULL, 0, &ncfg),
      "broken EGL on main X display\n");*/
    dconfigs = (EGLConfig*)malloc(sizeof(EGLConfig));
    ret = dfns.eglGetConfigs(ddisplay, NULL, 0, &ncfg);
    die_if(!ret, "broken EGL on main X display (getconfig)\n");
    ret = dfns.eglChooseConfig(ddisplay, attrs, dconfigs, 1, &ncfg);
    die_if(!ret || ncfg == 0, "broken EGL on main X display (chooseconfig)\n");

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
static bool test_drawpixels_fast(Display *dpy, EGLContext ctx)
{
  int width = 1920, height = 1080;
  int pbattrs[] = {GLX_PBUFFER_WIDTH, width, GLX_PBUFFER_HEIGHT, height, GLX_PRESERVED_CONTENTS, True, None};
  EGLSurface pbuffer = primus.dfns.glXCreatePbuffer(dpy, primus.dconfigs[0], pbattrs);
  primus.dfns.glXMakeCurrent(dpy, pbuffer, ctx);
  GLuint pbo;
  primus.dfns.glGenBuffers(1, &pbo);
  primus.dfns.glBindBuffer(GL_PIXEL_UNPACK_BUFFER_EXT, pbo);
  primus.dfns.glBufferData(GL_PIXEL_UNPACK_BUFFER_EXT, width*height*4, NULL, GL_STREAM_DRAW);
  void *pixeldata = malloc(width*height*4);

  double end = 0.2 + Profiler::get_timestamp();
  int iters = 0;
  do {
    primus.dfns.glBufferSubData(GL_PIXEL_UNPACK_BUFFER_EXT, 0, width*height*4, pixeldata);
    primus.dfns.glDrawPixels(width, height, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    primus.dfns.glXSwapBuffers(dpy, pbuffer);
    iters++;
  } while (end > Profiler::get_timestamp());

  free(pixeldata);
  primus.dfns.glDeleteBuffers(1, &pbo);
  primus.dfns.glXDestroyPbuffer(dpy, pbuffer);

  bool is_fast = iters >= 12;
  primus_perf("upload autodetection: will use %s path (%d iters)\n", is_fast ? "PBO" : "texture", iters);
  return is_fast;
}
#endif

//GLuint init_display_program()
//GLuint load_shader( GLenum type, const char *shaderSrc );

///
// Create a shader object, load the shader source, and
// compile the shader.
//
static GLuint load_shader( GLenum type, const char *shaderSrc )
{
   GLuint shader;
   GLint compiled;

   // Create the shader object
   shader = primus.dfns.glCreateShader ( type );

   // Load the shader source
   primus.dfns.glShaderSource ( shader, 1, &shaderSrc, NULL );
   
   // Compile the shader
   primus.dfns.glCompileShader ( shader );

   // Check the compile status
   primus.dfns.glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );

   if ( !compiled ) 
   {
      GLint infoLen = 0;

      primus.dfns.glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );
      
      if ( infoLen > 1 )
      {
          char* infoLog = (char*)malloc (sizeof(char) * infoLen );

         primus.dfns.glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
         printf ( "Error compiling shader:\n%s\n", infoLog );
         
         free ( infoLog );
      }

      primus.dfns.glDeleteShader ( shader );
      return 0;
   }

   return shader;
}

static GLuint init_display_program()
{
   const char* vShaderStr =  
      "attribute vec4 vPosition;    \n"
      "attribute vec2 vTexture;    \n"
      "varying vec2 vTexCoord;\n"
      "void main()                  \n"
      "{                            \n"
      "   gl_Position = vPosition;  \n"
      "   vTexCoord = vTexture;  \n"
      "}                            \n";
   
   const char* fShaderStr =  
      "precision mediump float;\n"
      "uniform sampler2D uTexture;\n"
      "varying vec2 vTexCoord;\n"
      "void main()                                  \n"
      "{                                            \n"
      "  gl_FragColor = texture2D(uTexture, vTexCoord);\n"
      "}                                            \n";

   GLuint vertexShader;
   GLuint fragmentShader;
   GLuint programObject;
   GLint linked;

   // Load the vertex/fragment shaders
   vertexShader = load_shader ( GL_VERTEX_SHADER, vShaderStr );
   fragmentShader = load_shader ( GL_FRAGMENT_SHADER, fShaderStr );

   // Create the program object
   programObject = primus.dfns.glCreateProgram ( );
   
if ( programObject == 0 ) {
printf("po=0\n");
      return 0;
}

   primus.dfns.glAttachShader ( programObject, vertexShader );
   primus.dfns.glAttachShader ( programObject, fragmentShader );

   // Bind vPosition to attribute 0   
   primus.dfns.glBindAttribLocation ( programObject, 0, "vPosition" );

   // Link the program
   primus.dfns.glLinkProgram ( programObject );

   // Check the link status
   primus.dfns.glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );

   if ( !linked ) 
   {
      GLint infoLen = 0;

      primus.dfns.glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );
      
      if ( infoLen > 1 )
      {
          char* infoLog = (char*)malloc (sizeof(char) * infoLen );

         primus.dfns.glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
         printf ( "Error linking program:\n%s\n", infoLog );            
         
         free ( infoLog );
      }

      primus.dfns.glDeleteProgram ( programObject );
      return 0;
   }

   primus.dfns.glClearColor ( 0.0f, 0.0f, 0.0f, 0.0f );
   return programObject;
}

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

static void* display_work(void *vd)
{
  printf("primus display_work\n");
  EGLSurface drawable = (EGLSurface)vd;
  DrawableInfo &di = primus.drawables[drawable];
  int width, height;
  GLuint program;
  static const float quad_vertex_coords[]  = {-1, -1, 0, -1,  1, 0, 1, 1, 0,
                                              -1, -1, 0,  1, -1, 0, 1, 1, 0};
  static const float quad_texture_coords[] = { 0, 0, 0, 1, 1, 1,
                                               0, 0, 1, 0, 1, 1};
  GLint iVertex, iTexture, iTextureUniform;
  GLuint textures[2] = {0};
  GLuint pbos[2] = {0};
  int ctex = 0;
  static const char *state_names[] = {"wait", "upload", "draw+swap", NULL};
  Profiler profiler("display", state_names);
  //Display *ddpy = XOpenDisplay(NULL);
  Display * ddpy = primus.ddpy;
  assert(di.kind == di.XWindow || di.kind == di.Window);
  XSelectInput(ddpy, di.window, StructureNotifyMask);
  note_geometry(ddpy, di.window, &width, &height);
  di.update_geometry(width, height);
  EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };  

  //EGLDisplay ddisplay = primus.dfns.eglGetDisplay((EGLNativeDisplayType)ddpy);
  EGLDisplay ddisplay = primus.ddisplay;

   EGLint majorVersion;
   EGLint minorVersion;

   EGLBoolean ret;

   /*ret = primus.dfns.eglInitialize(ddisplay, &majorVersion, &minorVersion);

  die_if(!ret,
  "failed to init EGL in display thread\n");*/

//   EGLConfig dcfg;
//1   match_config(1, di.config, &dcfg);

  EGLContext context = primus.dfns.eglCreateContext(ddisplay, primus.dconfigs[0], EGL_NO_CONTEXT, contextAttribs );
  die_if(!context,
    "failed to acquire rendering context for display thread\n");

//glXCreateNewContext(ddpy, primus.dconfigs[0], GLX_RGBA_TYPE, NULL, True);
  //FIXME
/*  die_if(!primus.dfns.glXIsDirect(ddpy, context),
    "failed to acquire direct rendering context for display thread\n");*/

/*
  if (!primus.dispmethod)
  primus.dispmethod = test_drawpixels_fast(ddpy, context) ? 2 : 1;*/
  primus.dispmethod = 1; /* Force texture */

  printf("primus display_work 2.0\n");

  printf("di.windowsurface=%p\n", di.windowsurface);

  ret = primus.dfns.eglMakeCurrent(ddisplay, di.windowsurface, di.windowsurface, context);

  //printf("eglMakeCurrent: %d\n", primus.dfns.eglGetError());

  die_if(!ret, "failed eglMakeCurrent in display\n");

  bool use_textures = (primus.dispmethod == 1);
  if (use_textures)
  {
      program = init_display_program();
      iVertex = primus.dfns.glGetAttribLocation(program, "vPosition");
      iTexture = primus.dfns.glGetAttribLocation(program, "vTexture");
      iTextureUniform = primus.dfns.glGetUniformLocation(program, "uTexture");
      //printf("iTexture=%d iTU=%d\n", iTexture, iTextureUniform);
      /* FIXME: Convert to shader */
      //primus.dfns.glVertexPointer  (2, GL_FLOAT, 0, quad_vertex_coords);
      //primus.dfns.glTexCoordPointer(2, GL_FLOAT, 0, quad_texture_coords);
      //primus.dfns.glEnableClientState(GL_VERTEX_ARRAY);
      //primus.dfns.glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    primus.dfns.glActiveTexture(GL_TEXTURE0);
    primus.dfns.glEnable(GL_TEXTURE_2D);
    primus.dfns.glGenTextures(2, textures);
  }
  else {
  printf("primus display_work 2.5\n");
    primus.dfns.glGenBuffers(2, pbos);
  }

  printf("primus display_work 3.0\n");

  for (;;)
  {
    sem_wait(&di.d.acqsem);
    profiler.tick(true);
    if (di.d.reinit)
    {
      if (di.d.reinit == di.SHUTDOWN)
      {
	if (use_textures)
            ;  //primus.dfns.glDeleteTextures(2, textures);
	else
	  primus.dfns.glDeleteBuffers(2, pbos);
	primus.dfns.eglMakeCurrent(ddisplay, 0, 0, NULL);
	primus.dfns.eglDestroyContext(ddisplay, context);
	XCloseDisplay(ddpy);
	sem_post(&di.d.relsem);
	return NULL;
      }
      di.d.reinit = di.NONE;
      profiler.width = width = di.width;
      profiler.height = height = di.height;
      primus.dfns.glViewport(0, 0, width, height);
      if (use_textures)
      {
	primus.dfns.glBindTexture(GL_TEXTURE_2D, textures[ctex ^ 1]);
	primus.dfns.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	primus.dfns.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	primus.dfns.glBindTexture(GL_TEXTURE_2D, textures[ctex]);
	primus.dfns.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	primus.dfns.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      }
      else
      {
	primus.dfns.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[ctex ^ 1]);
	primus.dfns.glBufferData(GL_PIXEL_UNPACK_BUFFER, width*height*4, NULL, GL_STREAM_DRAW);
	primus.dfns.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[ctex]);
	primus.dfns.glBufferData(GL_PIXEL_UNPACK_BUFFER, width*height*4, NULL, GL_STREAM_DRAW);
      }
      sem_post(&di.d.relsem);
      continue;
    }
    if (use_textures)
        primus.dfns.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, di.pixeldata);
    else
      primus.dfns.glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, width*height*4, di.pixeldata);
    if (!primus.sync)
      sem_post(&di.d.relsem); // Unlock as soon as possible
    profiler.tick();
    if (use_textures)
    {
        glClear(GL_COLOR_BUFFER_BIT);

         // Use the program object
        primus.dfns.glUseProgram ( program );

        primus.dfns.glViewport(0, 0, width, height);

        // Load the vertex data
       primus.dfns.glVertexAttribPointer ( iVertex, 3, GL_FLOAT, GL_FALSE, 0, quad_vertex_coords );
       primus.dfns.glEnableVertexAttribArray ( iVertex );

       primus.dfns.glEnableVertexAttribArray(iTexture);
       primus.dfns.glVertexAttribPointer(iTexture, 2, GL_FLOAT, GL_FALSE, 0, quad_texture_coords);

       primus.dfns.glUniform1i(iTextureUniform, 0);

       primus.dfns.glBindTexture(GL_TEXTURE_2D, textures[ctex ^= 1]);

       primus.dfns.glDrawArrays ( GL_TRIANGLES, 0, 6 );
    }
    else
    {
      //FIXME: Can't be done in OpenGL ES, it seems...
      //primus.dfns.glDrawPixels(width, height, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
      primus.dfns.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[ctex ^= 1]);
    }
    primus.dfns.eglSwapBuffers(ddisplay, di.windowsurface);
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
  int width, height;
  GLuint pbos[2] = {0};
  int cbuf = 0;
  unsigned sleep_usec = 0;
  static const char *state_names[] = {"app", "sleep", "map", "wait", NULL};
  Profiler profiler("readback", state_names);
  struct timespec tp;
  if (!primus.sync)
    sem_post(&di.d.relsem); // No PBO is mapped initially

   EGLint majorVersion;
   EGLint minorVersion;
   EGLBoolean ret;

/*   EGLBoolean ret = primus.afns.eglInitialize(primus.adisplay, &majorVersion, &minorVersion);
  die_if(!ret,
  "failed to init EGL in readback thread\n");*/

   //primus.afns.eglBindAPI(EGL_OPENGL_ES_API);

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
  primus.afns.glGenBuffers(2, &pbos[0]);
  primus.afns.glReadBuffer(GL_FRONT);

  ret = primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);

  printf("readback_work 2.0\n");
  for (;;)
  {
    sem_wait(&di.r.acqsem);
    profiler.tick(true);
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
      if (di.r.reinit == di.SHUTDOWN)
      {
	primus.afns.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[cbuf ^ 1]);
	primus.afns.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	primus.afns.glDeleteBuffers(2, &pbos[0]);
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
      primus.afns.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[cbuf ^ 1]);
      primus.afns.glBufferData(GL_PIXEL_PACK_BUFFER, width*height*4, NULL, GL_STREAM_READ);
      primus.afns.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[cbuf]);
      primus.afns.glBufferData(GL_PIXEL_PACK_BUFFER, width*height*4, NULL, GL_STREAM_READ);
    }
    primus.afns.glWaitSync(di.sync, 0, GL_TIMEOUT_IGNORED);
    primus.afns.glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (!primus.sync) {
        //sem_post(&di.r.relsem); // Unblock main thread as soon as possible
    }
    usleep(sleep_usec);
    profiler.tick();
    if (primus.sync == 1) // Get the previous framebuffer
      primus.afns.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[cbuf ^ 1]);
    double map_time = Profiler::get_timestamp();
    GLvoid *pixeldata = primus.afns.glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                     width*height*4, GL_MAP_READ_BIT);
    map_time = Profiler::get_timestamp() - map_time;
    sleep_usec = (map_time * 1e6 + sleep_usec) * primus.autosleep / 100;
    profiler.tick();
    clock_gettime(CLOCK_REALTIME, &tp);
    tp.tv_sec  += 1;

    static int x = 0;
    x++;
    unsigned int sum = 0; int i;
    for (i = 0; i < width*height; i++) {
        sum += ((unsigned int*)pixeldata)[i];
        //    ((unsigned int*)pixeldata)[i] = 0x00000000 + (x << 8) + ((i/width)*5)%256;
        //((unsigned int*)pixeldata)[i] = 0x00000000 + (x << 8) + ((i%width)*5)%256;
    }

    if (!primus.sync && sem_timedwait(&di.d.relsem, &tp))
      primus_warn("dropping a frame to avoid deadlock\n");
    else
    {
      di.pixeldata = pixeldata;
      sem_post(&di.d.acqsem);
      if (primus.sync)
      {
	sem_wait(&di.d.relsem);
//	sem_post(&di.r.relsem); // Unblock main thread only after D::work has completed
      }
      cbuf ^= 1;
      primus.afns.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[cbuf]);
    }
    primus.afns.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    //printf("pixeldatasum=%u\n", sum);
    //printf("Releasing egl context in readback\n");
    primus.afns.eglMakeCurrent(primus.adisplay, 0, 0, NULL);
    //printf("done\n");
    sem_post(&di.r.relsem); // Unblock main thread only after D::work has completed
    profiler.tick();
  }
  return NULL;
}

EGLBoolean eglBindAPI( 	EGLenum  api) {
    return primus.afns.eglBindAPI(api);
}

EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share_context,
                            EGLint const *attrib_list)
{
    printf("primus eglCreateContext\n");
    //EGLConfig acfg;
  //match_config(0, config, &acfg);
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
  EGLint pbattrs[] =
    {
        EGL_WIDTH, di.width,
        EGL_HEIGHT, di.height,
        EGL_NONE
    };
  //int pbattrs[] = {GLX_PBUFFER_WIDTH, di.width, GLX_PBUFFER_HEIGHT, di.height, GLX_PRESERVED_CONTENTS, True, None};
  EGLSurface surface = primus.afns.eglCreatePbufferSurface(dpy, di.config, pbattrs);
  printf("create_pbuffer %p %d %d config=%p %p\n", dpy, di.width, di.height, di.config, surface);

  EGLint val;

  primus.afns.eglQuerySurface(dpy, surface,  EGL_CONFIG_ID, &val);
  printf("eglQuerySurface=%d\n", val);

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
      printf("!known\n");
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

/* FIXME: This removes the need for passthru hacks I think */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
    printf("primus eglGetDisplay\n");
    /* FIXME: Why the hell does that even work?!?!?! */
    //primus.adisplay = primus.afns.eglGetDisplay((EGLNativeDisplayType)display_id);
    primus.ddpy = display_id;
    primus.ddisplay = primus.dfns.eglGetDisplay((EGLNativeDisplayType)display_id);

   EGLint majorVersion;
   EGLint minorVersion;
   EGLint ncfg;
   EGLBoolean ret;
    EGLint attrs[] =
        {
            EGL_RED_SIZE,       8,
            EGL_GREEN_SIZE,     8,
            EGL_BLUE_SIZE,      8,
            EGL_ALPHA_SIZE,     EGL_DONT_CARE,
            EGL_DEPTH_SIZE,     EGL_DONT_CARE,
            EGL_STENCIL_SIZE,   EGL_DONT_CARE,
            EGL_SAMPLE_BUFFERS, 0,
            EGL_NONE
        };


    ret = primus.dfns.eglInitialize(primus.ddisplay, &majorVersion, &minorVersion);
    die_if(!ret, "broken EGL on main X display (eglInitialize)\n");

    /* FIXME: Do we need the list of config? Or just the first one? */
/*    die_if(!eglGetConfigs(ddpy, NULL, 0, &ncfg),
      "broken EGL on main X display\n");*/
    primus.dconfigs = (EGLConfig*)malloc(sizeof(EGLConfig));
    ret = primus.dfns.eglGetConfigs(primus.ddisplay, NULL, 0, &ncfg);
    die_if(!ret, "broken EGL on main X display (getconfig)\n");
    ret = primus.dfns.eglChooseConfig(primus.ddisplay, attrs, primus.dconfigs, 1, &ncfg);
    die_if(!ret || ncfg == 0, "broken EGL on main X display (chooseconfig)\n");

    return primus.adisplay;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType win, EGLint const* attribList) {
    EGLConfig dcfg;
    match_config(1, config, &dcfg);
  EGLSurface surface = primus.dfns.eglCreateWindowSurface(primus.ddisplay,
                                                          //dcfg,
                                                          primus.dconfigs[0],
                                                          win, attribList);
  printf("surface=%p %p %lu %p\n", surface, dcfg, win, attribList);
  DrawableInfo &di = primus.drawables[surface];
  di.kind = di.Window;
  di.config = config;
  di.window = win;
  di.windowsurface = surface;
  printf("Noting geometry\n");
  note_geometry(primus.ddpy, win, &di.width, &di.height);
  printf("returning surface\n");
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
  return primus.dfns.eglDestroySurface(dpy, surface);
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attribList)
{
  EGLSurface pbuffer = primus.dfns.eglCreatePbufferSurface(dpy, primus.dconfigs[0], attribList);
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
        printf("Getting visual ID.\n");
          EGLConfig dcfg;
          match_config(1, config, &dcfg);
      return primus.dfns.eglGetConfigAttrib(primus.ddisplay, dcfg, attribute, value);
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
    printf("non-strict Calling %s\n", #name);                                            \
    void* val = primus.dfns.handle_valid() ? primus.dfns.dlsym(#name) : NULL; \
    printf("Val %p\n", val);                                            \
    return val;\
}
#include "egl-passthru.def"
#undef P
#endif
