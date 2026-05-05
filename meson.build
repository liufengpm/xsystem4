project('system4', 'c',
        meson_version: '>= 0.59',
        default_options : ['c_std=c11'])
add_project_arguments('-D_DEFAULT_SOURCE', language : 'c')
add_project_arguments('-DXSYS4_DATA_DIR="' + get_option('prefix') / get_option('datadir') / 'xsystem4"', language : 'c')

if get_option('buildtype').startswith('release')
    add_project_arguments('-DRELEASE', language : 'c')
endif

static_libs = false
if host_machine.system() == 'windows' and get_option('windows_static_deps')
    static_libs = true
endif

audio_backend = get_option('audio_backend')

zlib = dependency('zlib', static : static_libs)
libm = meson.get_compiler('c').find_library('m', required: false)
sdl2 = dependency('sdl2', static : static_libs)
ft2 = dependency('freetype2', static : static_libs)
ffi = dependency('libffi', static : static_libs)
tj = dependency('libturbojpeg', static : static_libs)
webp = dependency('libwebp', static : static_libs)
png = dependency('libpng', static : static_libs)
cglm = dependency('cglm', version : '>=0.9.2', fallback : ['cglm', 'cglm_dep'])
sndfile = dependency('sndfile', required : audio_backend == 'sndfile', static : static_libs)

avcodec = dependency('libavcodec', version : '>=59.37.100', required: audio_backend == 'ffmpeg', static : static_libs)
avformat = dependency('libavformat', required: audio_backend == 'ffmpeg', static : static_libs)
avutil = dependency('libavutil', version : '>=57.28.100', required: audio_backend == 'ffmpeg', static : static_libs)
swscale = dependency('libswscale', required: false, static : static_libs)
swresample = dependency('libswresample', required: audio_backend == 'ffmpeg', static : static_libs)

# The dependency version is not a reliable proxy for the supported GLES API
# level. ANGLE, for example, ships libGLESv2/libEGL with package versions
# that do not map cleanly to the exposed ES 3.x feature set.
gl_deps = []
if not get_option('opengles').disabled()
    gles = dependency('glesv2', required: false, static : static_libs)
    if gles.found()
        gl_deps = [gles]
        egl = dependency('egl', required : false, static : static_libs)
        if egl.found()
            gl_deps += [egl]
        endif
    elif host_machine.system() == 'windows'
        # MSYS2 ANGLE currently exposes a single angleproject.pc entry that
        # carries both libEGL and libGLESv2, so probe that before falling back.
        angle = dependency('angleproject', required: get_option('opengles').enabled(), static : static_libs)
        if angle.found()
            gl_deps = [angle]
        elif get_option('opengles').enabled()
            error('OpenGL ES requested, but neither glesv2 nor angleproject could be resolved')
        endif
    elif get_option('opengles').enabled()
        error('OpenGL ES requested, but glesv2 could not be resolved')
    endif
endif

if gl_deps.length() > 0
    add_project_arguments('-DUSE_GLES', language : 'c')
else
    gl = dependency('GL')
    glew = dependency('glew')
    gl_deps = [gl, glew]
endif

chibi = dependency('chibi-scheme', required : false, static : static_libs)
readline = dependency('readline', required : false, static : static_libs)

flex = find_program('flex')
bison = find_program('bison')

incdir = include_directories('include')

libsys4_default_options = []
if get_option('windows_static_deps')
    libsys4_default_options += ['windows_static_deps=true']
else
    libsys4_default_options += ['windows_static_deps=false']
endif

libsys4_proj = subproject('libsys4', default_options : libsys4_default_options)
libsys4_dep = libsys4_proj.get_variable('libsys4_dep')

subdir('src')

install_subdir('shaders', install_dir : get_option('datadir') / 'xsystem4')
install_subdir('fonts', install_dir : get_option('datadir') / 'xsystem4')
install_data('debugger.scm', install_dir : get_option('datadir') / 'xsystem4')
