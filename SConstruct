# vim: syntax=python
import os

screen_start = 'C000'
sprite_start = 'C400'
character_start = 'D800'

if 'CC65_HOME' in os.environ:
    cc65_home = os.environ['CC65_HOME']
else:
    cc65_home = ''

if 'DISPLAY' in os.environ:
    display = os.environ['DISPLAY']
else:
    display = ''

print(cc65_home)
print(os.environ['PATH'])

env = Environment(
    BUILDERS = {},
    ENV = {
        'PATH': os.environ["PATH"],
        'CC65_HOME': cc65_home,
        'DISPLAY': display,
    },
    AS = 'ca65',
    ASFLAGS = ['-t', 'c64', '-g'],
    CC = 'cl65',
    CFLAGS = ['-DSCREEN_START=0x'+screen_start, '-DSPRITE_START=0x'+sprite_start, '-DCHARACTER_START=0x'+character_start, '-O', '-Osir', '-t', 'c64', '-C', 'c64.cfg', '-g', '-Wc', '--debug-tables', '-Wc', '${SOURCE}.tab'],
    LINK = 'cl65',
    LINKFLAGS = ['-g', '-C', 'c64.cfg', '-D__HIMEM__=$' + screen_start, '-Wl', '--dbgfile,build/msprite.dbg', '-Wl', '-Lnbuild/msprite.lbl', '-Wl', '--mapfile,build/msprite.map']
)

prg = env.Program(target=["build/msprite.prg", "build/msprite.map", "build/msprite.dbg", "build/msprite.lbl"], source=[Glob('src/*.c'), Glob('src/*_asm.s')])

sprites = Glob('res/sprites/*.spd')

disk_files = []
disk_files.append(prg[0])
disk_files.append(sprites)

def disk_func(target, source, env):
    if not target[0].exists():
        env.Execute('c1541 -format "canada,01" d64 "%s"' % target[0])
    changes = []
    for src in source:
        basename = os.path.basename(str(src))
        typename = 's'
        if basename.endswith('prg'):
            typename = 'p'
        changes.append(""" -delete '%s' -write '%s' '%s,%s'""" % (basename, str(src), basename, typename))
    env.Execute("""c1541 -attach '%s' %s """ % (str(target[0]), ''.join(changes)))

disk_image = env.Command(target=["build/msprite.d64"], source=disk_files, action=disk_func)

env.Alias('build', disk_image)

Default(disk_image)
