project('libsys4', 'c',
        default_options : ['c_std=c11', 'default_library=static'])
add_project_arguments('-D_DEFAULT_SOURCE', language : 'c')

zlib = dependency('zlib')
libm = meson.get_compiler('c').find_library('m', required: false)
tj = dependency('libturbojpeg')
webp = dependency('libwebp')
png = dependency('libpng')

flex = find_program('flex')
bison = find_program('bison')

flexgen = generator(flex,
                    output : '@BASENAME@.yy.c',
                    arguments : ['-o', '@OUTPUT@', '@INPUT@'])

bisongen = generator(bison,
                     output: ['@BASENAME@.tab.c', '@BASENAME@.tab.h'],
                     arguments : ['--verbose', '@INPUT@', '--defines=@OUTPUT1@', '--output=@OUTPUT0@'])

inc = include_directories('include')
local_inc = include_directories('src')

# sources for libsys4.a
system4 = ['src/acx.c',
           'src/afa.c',
           'src/afa3.c',
           'src/ain.c',
           'src/ajp.c',
           'src/ald.c',
           'src/alk.c',
           'src/archive.c',
           'src/buffer.c',
           'src/cg.c',
           'src/dasm.c',
           'src/dcf.c',
           'src/dlf.c',
           'src/ex.c',
           'src/file.c',
           'src/flat.c',
           'src/fnl.c',
           'src/hashtable.c',
           'src/ini.c',
           'src/instructions.c',
           'src/jpeg.c',
           'src/mt19937int.c',
           'src/pms.c',
           'src/png.c',
           'src/qnt.c',
           'src/string.c',
           'src/system.c',
           'src/utfsjis.c',
           'src/webp.c',
]

system4 += flexgen.process('src/ini_lexer.l')
system4 += bisongen.process('src/ini_parser.y')

libsys4 = library('sys4', system4,
                  dependencies : [libm, zlib, tj, webp, png],
                  include_directories : [inc, local_inc],
                  install : true)

libsys4_dep = declare_dependency(include_directories : inc,
                                 link_with : libsys4)
