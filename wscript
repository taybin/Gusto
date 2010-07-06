#!/usr/bin/env python
import autowaf
import Options
import os
import commands
import re
import string
import subprocess
import sys

# Variables for 'waf dist'
VERSION = '3.0pre0'
APPNAME = 'ardour'

# Mandatory variables
srcdir = '.'
blddir = 'build'

children = [
	'libs/pbd',
	'libs/midi++2',
	'libs/evoral',
	'libs/vamp-sdk',
	'libs/vamp-plugins',
	'libs/taglib',
	'libs/rubberband',
	'libs/surfaces',
	'libs/ardour',
	'libs/gtkmm2ext',
	'libs/clearlooks-newer',
	'libs/audiographer',
	'gtk2_ardour'
]


i18n_children = [
	'gtk2_ardour',
	'libs/ardour'
]

# Version stuff

def fetch_svn_revision (path):
	cmd = "LANG= svn info " + path + " | awk '/^Revision:/ { print $2}'"
	return commands.getoutput(cmd)

def fetch_gcc_version ():
	cmd = "LANG= gcc --version"
	output = commands.getoutput(cmd).splitlines()
        version = output[0].split(' ')[2].split('.')
	return version

def fetch_git_revision (path):
	cmd = "LANG= git log --abbrev HEAD^..HEAD " + path
	output = commands.getoutput(cmd).splitlines()
	rev = output[0].replace ("commit", "git")[0:10]
	for line in output:
		try:
			if "git-svn-id" in line:
				line = line.split('@')[1].split(' ')
				rev = line[0]
		except:
			pass
	return rev

def fetch_bzr_revision (path):
	cmd = subprocess.Popen("LANG= bzr log -l 1 " + path, stdout=subprocess.PIPE, shell=True)
	out = cmd.communicate()[0]
	svn = re.search('^svn revno: [0-9]*', out, re.MULTILINE)
	str = svn.group(0)
	chars = 'svnreio: '
	return string.lstrip(str, chars)

def create_stored_revision():
	rev = ""
	if os.path.exists('.svn'):
		rev = fetch_svn_revision('.');
	elif os.path.exists('.git'):
		rev = fetch_git_revision('.');
	elif os.path.exists('.bzr'):
		rev = fetch_bzr_revision('.');
		print "Revision: " + rev;
	elif os.path.exists('libs/ardour/svn_revision.cc'):
		print "Using packaged svn revision"
		return
	else:
		print "Missing libs/ardour/svn_revision.cc.  Blame the packager."
		sys.exit(-1)

	try:
		text =  '#include "ardour/svn_revision.h"\n'
		text += 'namespace ARDOUR { const char* svn_revision = \"' + rev + '\"; }\n'
		print 'Writing svn revision info to libs/ardour/svn_revision.cc'
		o = file('libs/ardour/svn_revision.cc', 'w')
		o.write(text)
		o.close()
	except IOError:
		print 'Could not open libs/ardour/svn_revision.cc for writing\n'
		sys.exit(-1)

def set_compiler_flags (conf,opt):
	#
	# Compiler flags and other system-dependent stuff
	#

	build_host_supports_sse = False
	optimization_flags = []
	if opt.gprofile:
		debug_flags = [ '-pg' ]
	else:
		debug_flags = [ ] # waf adds -O0 -g itself. thanks waf!

	# guess at the platform, used to define compiler flags

	config_guess = os.popen("tools/config.guess").read()[:-1]

	config_cpu = 0
	config_arch = 1
	config_kernel = 2
	config_os = 3
	config = config_guess.split ("-")

	print "system triple: " + config_guess

	# Autodetect
	if opt.dist_target == 'auto':
		if config[config_arch] == 'apple':
			# The [.] matches to the dot after the major version, "." would match any character
			if re.search ("darwin[0-7][.]", config[config_kernel]) != None:
				conf.define ('build_target', 'panther')
			elif re.search ("darwin8[.]", config[config_kernel]) != None:
				conf.define ('build_target', 'tiger')
			else:
				conf.define ('build_target', 'leopard')
		else:
			if re.search ("x86_64", config[config_cpu]) != None:
				conf.define ('build_target', 'x86_64')
			elif re.search("i[0-5]86", config[config_cpu]) != None:
				conf.define ('build_target', 'i386')
			elif re.search("powerpc", config[config_cpu]) != None:
				conf.define ('build_target', 'powerpc')
			else:
				conf.define ('build_target', 'i686')
	else:
		conf.define ('build_target', opt.dist_target)

	if config[config_cpu] == 'powerpc' and conf.env['build_target'] != 'none':
		#
		# Apple/PowerPC optimization options
		#
		# -mcpu=7450 does not reliably work with gcc 3.*
		#
		if opt.dist_target == 'panther' or opt.dist_target == 'tiger':
			if config[config_arch] == 'apple':
				# optimization_flags.extend ([ "-mcpu=7450", "-faltivec"])
				# to support g3s but still have some optimization for above
				optimization_flags.extend ([ "-mcpu=G3", "-mtune=7450"])
			else:
				optimization_flags.extend ([ "-mcpu=7400", "-maltivec", "-mabi=altivec"])
		else:
			optimization_flags.extend([ "-mcpu=750", "-mmultiple" ])
		optimization_flags.extend (["-mhard-float", "-mpowerpc-gfxopt"])
		optimization_flags.extend (["-Os"])

	elif ((re.search ("i[0-9]86", config[config_cpu]) != None) or (re.search ("x86_64", config[config_cpu]) != None)) and conf.env['build_target'] != 'none':
    
    
		#
		# ARCH_X86 means anything in the x86 family from i386 to x86_64
		# USE_X86_64_ASM is used to distingush 32 and 64 bit assembler
		#

		if (re.search ("(i[0-9]86|x86_64)", config[config_cpu]) != None):
			debug_flags.append ("-DARCH_X86")
			optimization_flags.append ("-DARCH_X86")
    
		if config[config_kernel] == 'linux' :

			#
			# determine processor flags via /proc/cpuinfo
			#
        
			if conf.env['build_target'] != 'i386':
            
				flag_line = os.popen ("cat /proc/cpuinfo | grep '^flags'").read()[:-1]
				x86_flags = flag_line.split (": ")[1:][0].split ()
            
				if "mmx" in x86_flags:
					optimization_flags.append ("-mmmx")
				if "sse" in x86_flags:
					build_host_supports_sse = True
				if "3dnow" in x86_flags:
					optimization_flags.append ("-m3dnow")
            
			if config[config_cpu] == "i586":
				optimization_flags.append ("-march=i586")
			elif config[config_cpu] == "i686":
				optimization_flags.append ("-march=i686")
    
		if ((conf.env['build_target'] == 'i686') or (conf.env['build_target'] == 'x86_64')) and build_host_supports_sse:
			optimization_flags.extend (["-msse", "-mfpmath=sse", "-DUSE_XMMINTRIN"])
			debug_flags.extend (["-msse", "-mfpmath=sse", "-DUSE_XMMINTRIN"])

	# end of processor-specific section

	# optimization section
	if conf.env['FPU_OPTIMIZATION']:
		if conf.env['build_target'] == 'tiger' or conf.env['build_target'] == 'leopard':
			optimization_flags.append ("-DBUILD_VECLIB_OPTIMIZATIONS");
			debug_flags.append ("-DBUILD_VECLIB_OPTIMIZATIONS");
			conf.env.append_value('LINKFLAGS', "-framework Accelerate")
		elif conf.env['build_target'] == 'i686' or conf.env['build_target'] == 'x86_64':
			optimization_flags.append ("-DBUILD_SSE_OPTIMIZATIONS")
			debug_flags.append ("-DBUILD_SSE_OPTIMIZATIONS")
		elif conf.env['build_target'] == 'x86_64':
			optimization_flags.append ("-DUSE_X86_64_ASM")
			debug_flags.append ("-DUSE_X86_64_ASM")
		if not build_host_supports_sse:
			print "\nWarning: you are building Ardour with SSE support even though your system does not support these instructions. (This may not be an error, especially if you are a package maintainer)"
	
	# check this even if we aren't using FPU optimization
	if conf.check_cc(function_name='posix_memalign', header_name='stdlib.h', ccflags='-D_XOPEN_SOURCE=600') == False:
		optimization_flags.append("-DNO_POSIX_MEMALIGN")

	# end optimization section
			
	#
	# no VST on x86_64
	#
	    
	if conf.env['build_target'] == 'x86_64' and opt.vst:
		print "\n\n=================================================="
		print "You cannot use VST plugins with a 64 bit host. Please run waf with --vst=0"
		print "\nIt is theoretically possible to build a 32 bit host on a 64 bit system."
		print "However, this is tricky and not recommended for beginners."
		sys.exit (-1)

	#
	# a single way to test if we're on OS X
	#

	if conf.env['build_target'] in ['panther', 'tiger', 'leopard' ]:
		conf.define ('IS_OSX', 1)
		# force tiger or later, to avoid issues on PPC which defaults
		# back to 10.1 if we don't tell it otherwise.
		conf.env.append_value('CCFLAGS', "-DMAC_OS_X_VERSION_MIN_REQUIRED=1040")

	else:
		conf.define ('IS_OSX', 0)

	#
	# save off guessed arch element in an env
	#
	conf.define ('CONFIG_ARCH', config[config_arch])

	#
	# ARCH="..." overrides all
	#

	if opt.arch != None:
		optimization_flags = opt.arch.split()
		
	#
	# prepend boiler plate optimization flags that work on all architectures
	#

	optimization_flags[:0] = [
		"-O3",
		"-fomit-frame-pointer",
		"-ffast-math",
		"-fstrength-reduce",
		"-pipe"
		]

	if opt.debug:
		conf.env.append_value('CCFLAGS', debug_flags)
		conf.env.append_value('CXXFLAGS', debug_flags)
		conf.env.append_value('LINKFLAGS', debug_flags)
	else:
		conf.env.append_value('CCFLAGS', optimization_flags)
		conf.env.append_value('CXXFLAGS', optimization_flags)
		conf.env.append_value('LINKFLAGS', optimization_flags)

	if opt.stl_debug:
		conf.env.append_value('CXXFLAGS', "-D_GLIBCXX_DEBUG")

	if opt.universal:
		conf.env.append_value('CCFLAGS', "-arch i386 -arch ppc")
		conf.env.append_value('CXXFLAGS', "-arch i386 -arch ppc")
		conf.env.append_value('LINKFLAGS', "-arch i386 -arch ppc")

	#
	# warnings flags
	#

	conf.env.append_value('CCFLAGS', "-Wall")
	conf.env.append_value('CXXFLAGS', [ '-Wall', '-Woverloaded-virtual'])

	if opt.extra_warn:
		flags = [ '-Wextra' ]
		conf.env.append_value('CCFLAGS', flags )
		conf.env.append_value('CXXFLAGS', flags )


	#
	# more boilerplate
	#

	conf.env.append_value('CCFLAGS', [ '-D_LARGEFILE64_SOURCE', '-D_LARGEFILE_SOURCE' ])
	conf.env.append_value('CCFLAGS', [ '-D_FILE_OFFSET_BITS=64', '-D_FILE_OFFSET_BITS=64' ])
	conf.env.append_value('CXXFLAGS', [ '-D_LARGEFILE64_SOURCE', '-D_LARGEFILE_SOURCE' ])
	conf.env.append_value('CXXFLAGS', [ '-D_FILE_OFFSET_BITS=64', '-D_FILE_OFFSET_BITS=64' ])
	if opt.nls:
		conf.env.append_value('CXXFLAGS', '-DENABLE_NLS')
		conf.env.append_value('CCFLAGS', '-DENABLE_NLS')


#----------------------------------------------------------------

# Waf stages

def set_options(opt):
	autowaf.set_options(opt)
	opt.add_option('--program-name', type='string', action='store', default='Ardour', dest='program_name',
			help='The user-visible name of the program being built')
	opt.add_option('--arch', type='string', action='store', dest='arch',
			help='Architecture-specific compiler flags')
	opt.add_option('--boost-sp-debug', action='store_true', default=False, dest='boost_sp_debug',
			help='Compile with Boost shared pointer debugging')
	opt.add_option('--audiounits', action='store_true', default=False, dest='audiounits',
			help='Compile with Apple\'s AudioUnit library (experimental)')
	opt.add_option('--coreaudio', action='store_true', default=False, dest='coreaudio',
			help='Compile with Apple\'s CoreAudio library')
	opt.add_option('--dist-target', type='string', default='auto', dest='dist_target',
			help='Specify the target for cross-compiling [auto,none,x86,i386,i686,x86_64,powerpc,tiger,leopard]')
	opt.add_option('--extra-warn', action='store_true', default=False, dest='extra_warn',
			help='Build with even more compiler warning flags')
	opt.add_option('--fpu-optimization', action='store_true', default=True, dest='fpu_optimization',
			help='Build runtime checked assembler code (default)')
	opt.add_option('--no-fpu-optimization', action='store_false', dest='fpu_optimization')
	opt.add_option('--freedesktop', action='store_true', default=False, dest='freedesktop',
			help='Install MIME type, icons and .desktop file as per freedesktop.org standards')
	opt.add_option('--freesound', action='store_true', default=False, dest='freesound',
			help='Include Freesound database lookup')
	opt.add_option('--gprofile', action='store_true', default=False, dest='gprofile',
			help='Compile for use with gprofile')
	opt.add_option('--gtkosx', action='store_true', default=False, dest='gtkosx',
			help='Compile for use with GTK-OSX, not GTK-X11')
	opt.add_option('--lv2', action='store_true', default=False, dest='lv2',
			help='Compile with support for LV2 (if slv2 is available)')
	opt.add_option('--nls', action='store_true', default=True, dest='nls',
			help='Enable i18n (native language support) (default)')
	opt.add_option('--no-nls', action='store_false', dest='nls')
	opt.add_option('--stl-debug', action='store_true', default=False, dest='stl_debug',
			help='Build with debugging for the STL')
	opt.add_option('--test', action='store_true', default=False, dest='build_tests', 
			help="Build unit tests")
	opt.add_option('--tranzport', action='store_true', default=False, dest='tranzport',
			help='Compile with support for Frontier Designs Tranzport (if libusb is available)')
	opt.add_option('--universal', action='store_true', default=False, dest='universal',
			help='Compile as universal binary (requires that external libraries are universal)')
	opt.add_option('--versioned', action='store_true', default=False, dest='versioned',
			help='Add revision information to executable name inside the build directory')
	opt.add_option('--vst', action='store_true', default=False, dest='vst',
			help='Compile with support for VST')
	opt.add_option('--wiimote', action='store_true', default=False, dest='wiimote',
			help='Build the wiimote control surface')
	opt.add_option('--windows-key', type='string', action='store', dest='windows_key', default='Mod4><Super',
		       help='X Modifier(s) (Mod1,Mod2, etc) for the Windows key (X11 builds only). ' +
		       'Multiple modifiers must be separated by \'><\'')

	for i in children:
		opt.sub_options(i)

def sub_config_and_use(conf, name, has_objects = True):
	conf.sub_config(name)
	autowaf.set_local_lib(conf, name, has_objects)

def configure(conf):
	create_stored_revision()
        conf.env['VERSION'] = VERSION
	autowaf.set_recursive()
	autowaf.configure(conf)

	gcc_versions = fetch_gcc_version()
        if Options.options.debug and gcc_versions[0] == '4' and gcc_versions[1] > '4':
                print 'Version 4.5 of gcc is not ready for use when compiling Ardour with optimization.'
                print 'Please use a different version or re-configure with --debug'
                exit (1)

	if sys.platform == 'darwin':
		#
		#	Define OSX as a uselib to use when compiling
		#	on Darwin to add all applicable flags at once
		#
		conf.env.append_value('CXXFLAGS_OSX', "-mmacosx-version-min=10.4")
		conf.env.append_value('CCFLAGS_OSX', "-mmacosx-version-min=10.4")
		conf.env.append_value('CXXFLAGS_OSX', "-isysroot /Developer/SDKs/MacOSX10.4u.sdk")
		conf.env.append_value('CCFLAGS_OSX', "-isysroot /Developer/SDKs/MacOSX10.4u.sdk")
		conf.env.append_value('LINKFLAGS_OSX', "-mmacosx-version-min=10.4")
		conf.env.append_value('LINKFLAGS_OSX', "-isysroot /Developer/SDKs/MacOSX10.4u.sdk")

		conf.env.append_value('LINKFLAGS_OSX', "-sysroot /Developer/SDKs/MacOSX10.4u.sdk")
		conf.env.append_value('LINKFLAGS_OSX', "-F/System/Library/Frameworks")

		conf.env.append_value('CXXFLAGS_OSX', "-msse")
		conf.env.append_value('CCFLAGS_OSX', "-msse")
		conf.env.append_value('CXXFLAGS_OSX', "-msse2")
		conf.env.append_value('CCFLAGS_OSX', "-msse2")
		#
		#	TODO: The previous sse flags NEED to be based
		#	off processor type.  Need to add in a check
		#	for that.
		#

		conf.env.append_value('CPPPATH_OSX', "/System/Library/Frameworks/")
		conf.env.append_value('CPPPATH_OSX', "/usr/include/")
		conf.env.append_value('CPPPATH_OSX', "/usr/include/c++/4.0.0")
		conf.env.append_value('CPPPATH_OSX', "/usr/include/c++/4.0.0/i686-apple-darwin8/")
		#
		#	TODO: Fix the above include path, it needs to be
		#	defined based off what is read in the configuration
		#	stage about the machine(PPC, X86, X86_64, etc.)
		#
		conf.env.append_value('CPPPATH_OSX', "/usr/lib/gcc/i686-apple-darwin9/4.0.1/include/")
		#
		#	TODO: Likewise this needs to be defined not only
		#	based off the machine characteristics, but also
		#	based off the version of GCC being used.
		#

		conf.env.append_value('FRAMEWORK_OSX', ['CoreFoundation'])

		conf.env.append_value('LINKFLAGS_OSX', ['-undefined', 'suppress'])
		conf.env.append_value('LINKFLAGS_OSX', "-flat_namespace")
		#
		#	The previous 2 flags avoid circular dependencies
		#	between libardour and libardour_cp on OS X.
		#	ld reported -undefined suppress as an unknown option
		#	in one of the tests ran, removing it for the moment
		#
		conf.env.append_value('CXXFLAGS_OSX', "-F/System/Library/Frameworks")
		conf.env.append_value('CCFLAGS_OSX', "-F/System/Library/Frameworks")

		# GTKOSX only builds on darwin anyways
		if Options.options.gtkosx:
			#
			#	Define Include Paths for GTKOSX
			#
			conf.env.append_value('CXXFLAGS_GTKOSX', '-DTOP_MENUBAR')
			conf.env.append_value('CXXFLAGS_GTKOSX', '-DGTKOSX')
			conf.env.append_value('LINKFLAGS_GTKOSX', "-framework AppKit")
			conf.env.append_value('LINKFLAGS_GTKOSX', "-Xlinker -headerpad")
			conf.env.append_value('LINKFLAGS_GTKOSX', "-Xlinker 2048")
			conf.env.append_value('CPPPATH_GTKOSX', "/System/Library/Frameworks/CoreServices.framework/Frameworks/CarbonCore.framework/Headers/")
	
		if Options.options.coreaudio:
			conf.check_cc (header_name = '/System/Library/Frameworks/CoreAudio.framework/Headers/CoreAudio.h',
			               define_name = 'HAVE_COREAUDIO', linkflags = ['-framework CoreAudio'],
			               uselib_store="COREAUDIO")
			conf.check_cxx (header_name = '/System/Library/Frameworks/AudioToolbox.framework/Headers/ExtendedAudioFile.h',
			                linkflags = [ '-framework AudioToolbox' ], uselib_store="COREAUDIO")
			conf.check_cc (header_name = '/System/Library/Frameworks/CoreServices.framework/Headers/CoreServices.h',
			               linkflags = ['-framework CoreServices'], uselib_store="COREAUDIO")

		if Options.options.audiounits:
			#conf.env.append_value('CXXFLAGS_AUDIOUNIT', "-DHAVE_AUDIOUNITS")
			conf.env.append_value('FRAMEWORK_AUDIOUNIT', ['AudioToolbox'])
			conf.env.append_value('FRAMEWORK_AUDIOUNIT', ['CoreServices'])
			conf.check_cc (header_name = '/System/Library/Frameworks/AudioUnit.framework/Headers/AudioUnit.h',
			               define_name = 'HAVE_AUDIOUNITS', linkflags = [ '-framework AudioUnit' ],
			               uselib_store="AUDIOUNIT")

	if Options.options.boost_sp_debug:
		conf.env.append_value('CXXFLAGS', '-DBOOST_SP_ENABLE_DEBUG_HOOKS')

	autowaf.check_header(conf, 'boost/signals2.hpp', mandatory = True)

	autowaf.check_header(conf, 'jack/session.h', define="JACK_SESSION")

	conf.check_cc(fragment = "#include <boost/version.hpp>\nint main(void) { return (BOOST_VERSION >= 103900 ? 0 : 1); }\n",
		      execute = "1",
		      mandatory = True,
		      msg = 'Checking for boost library >= 1.39',
		      okmsg = 'ok',
		      errmsg = 'too old\nPlease install boost version 1.39 or higher.')

	autowaf.check_pkg(conf, 'cppunit', uselib_store='CPPUNIT', atleast_version='1.12.0', mandatory=False)
	autowaf.check_pkg(conf, 'glib-2.0', uselib_store='GLIB', atleast_version='2.2')
	autowaf.check_pkg(conf, 'gthread-2.0', uselib_store='GTHREAD', atleast_version='2.2')
	autowaf.check_pkg(conf, 'glibmm-2.4', uselib_store='GLIBMM', atleast_version='2.14.0')
	if sys.platform == 'darwin':
		sub_config_and_use(conf, 'libs/appleutility')
	for i in children:
		sub_config_and_use(conf, i)

	# Fix utterly braindead FLAC include path to not smash assert.h
	conf.env['CPPPATH_FLAC'] = []

	# Tell everyone that this is a waf build

	conf.env.append_value('CCFLAGS', '-DWAF_BUILD')
	conf.env.append_value('CXXFLAGS', '-DWAF_BUILD')
	
	autowaf.print_summary(conf)
	opts = Options.options
	autowaf.display_header('Ardour Configuration')
	autowaf.display_msg(conf, 'Build Target', conf.env['build_target'])
	autowaf.display_msg(conf, 'Architecture flags', opts.arch)
	autowaf.display_msg(conf, 'Aubio', bool(conf.env['HAVE_AUBIO']))
	autowaf.display_msg(conf, 'AudioUnits', opts.audiounits)
	autowaf.display_msg(conf, 'CoreAudio', bool(conf.env['HAVE_COREAUDIO']))
	if bool(conf.env['HAVE_COREAUDIO']):
		conf.define ('COREAUDIO', 1)
	if opts.audiounits:
		conf.define('AUDIOUNITS',1)
	autowaf.display_msg(conf, 'FPU Optimization', opts.fpu_optimization)
	if opts.fpu_optimization:
		conf.define('FPU_OPTIMIZATION', 1)
	autowaf.display_msg(conf, 'Freedesktop Files', opts.freedesktop)
	autowaf.display_msg(conf, 'Freesound', opts.freesound)
	if opts.freesound:
		conf.define('FREESOUND',1)
	autowaf.display_msg(conf, 'GtkOSX', opts.gtkosx)
	if opts.gtkosx:
		conf.define ('GTKOSX', 1)
	autowaf.display_msg(conf, 'LV2 Support', bool(conf.env['HAVE_SLV2']))
	autowaf.display_msg(conf, 'Rubberband', bool(conf.env['HAVE_RUBBERBAND']))
	autowaf.display_msg(conf, 'Samplerate', bool(conf.env['HAVE_SAMPLERATE']))
	autowaf.display_msg(conf, 'Soundtouch', bool(conf.env['HAVE_SOUNDTOUCH']))
	autowaf.display_msg(conf, 'Translation', opts.nls)
	if opts.nls:
		conf.define ('ENABLE_NLS', 1)
	autowaf.display_msg(conf, 'Tranzport', opts.tranzport)
	if opts.build_tests:
		conf.env['BUILD_TESTS'] = opts.build_tests
		autowaf.display_msg(conf, 'Unit Tests', bool(conf.env['BUILD_TESTS']) and bool (conf.env['HAVE_CPPUNIT']))
	if opts.tranzport:
		conf.define('TRANZPORT', 1)
	autowaf.display_msg(conf, 'Universal Binary', opts.universal)
	autowaf.display_msg(conf, 'VST Support', opts.vst)
	if opts.vst:
		conf.define('VST_SUPPORT', 1)
	if bool(conf.env['JACK_SESSION']):
		conf.define ('HAVE_JACK_SESSION', 1)
	autowaf.display_msg(conf, 'Wiimote Support', opts.wiimote)
	if opts.wiimote:
		conf.define('WIIMOTE',1)
	if opts.windows_key:
		conf.define('WINDOWS_KEY', opts.windows_key)
	autowaf.display_msg(conf, 'Windows Key', opts.windows_key)
        conf.env['PROGRAM_NAME'] = opts.program_name
        autowaf.display_msg(conf, 'Program Name', opts.program_name)

	set_compiler_flags (conf, Options.options)

	autowaf.display_msg(conf, 'C Compiler flags', conf.env['CCFLAGS'])
	autowaf.display_msg(conf, 'C++ Compiler flags', conf.env['CXXFLAGS'])

	# and dump the same stuff to a file for use in the build

	config_text = open ('libs/ardour/config_text.cc',"w")
	config_text.write ('#include "ardour/ardour.h"\n\nnamespace ARDOUR {\nconst char* const ardour_config_info = "\\n\\\n')
	config_text.write ("Install prefix "); config_text.write (str (conf.env['PREFIX'])); config_text.write ("\\n\\\n")
	config_text.write ("Debuggable build "); config_text.write (str (str(conf.env['DEBUG']))); config_text.write ("\\n\\\n")
	config_text.write ("Strict compiler flags "); config_text.write (str (str(conf.env['STRICT']))); config_text.write ("\\n\\\n")
	config_text.write ("Build documentation "); config_text.write (str (str(conf.env['BUILD_DOCS']))); config_text.write ("\\n\\\n")
	config_text.write ('Build Target '); config_text.write (str (conf.env['build_target'])); config_text.write ("\\n\\\n")
	config_text.write ('Architecture flags '); config_text.write (str (opts.arch)); config_text.write ("\\n\\\n")
	config_text.write ('Aubio '); config_text.write (str (bool(conf.env['HAVE_AUBIO']))); config_text.write ("\\n\\\n")
	config_text.write ('AudioUnits '); config_text.write (str (opts.audiounits)); config_text.write ("\\n\\\n")
	config_text.write ('CoreAudio '); config_text.write (str (bool(conf.env['HAVE_COREAUDIO']))); config_text.write ("\\n\\\n")
	config_text.write ('FPU Optimization '); config_text.write (str (opts.fpu_optimization)); config_text.write ("\\n\\\n")
	config_text.write ('Freedesktop Files '); config_text.write (str (opts.freedesktop)); config_text.write ("\\n\\\n")
	config_text.write ('Freesound '); config_text.write (str (opts.freesound)); config_text.write ("\\n\\\n")
	config_text.write ('GtkOSX '); config_text.write (str (opts.gtkosx)); config_text.write ("\\n\\\n")
	config_text.write ('LV2 Support '); config_text.write (str (bool(conf.env['HAVE_SLV2']))); config_text.write ("\\n\\\n")
	config_text.write ('Rubberband '); config_text.write (str (bool(conf.env['HAVE_RUBBERBAND']))); config_text.write ("\\n\\\n")
	config_text.write ('Samplerate '); config_text.write (str (bool(conf.env['HAVE_SAMPLERATE']))); config_text.write ("\\n\\\n")
	config_text.write ('Soundtouch '); config_text.write (str (bool(conf.env['HAVE_SOUNDTOUCH']))); config_text.write ("\\n\\\n")
	config_text.write ('Translation '); config_text.write (str (opts.nls)); config_text.write ("\\n\\\n")
	config_text.write ('Tranzport '); config_text.write (str (opts.tranzport)); config_text.write ("\\n\\\n")
	config_text.write ('Universal Binary '); config_text.write (str (opts.universal)); config_text.write ("\\n\\\n")
	config_text.write ('VST Support '); config_text.write (str (opts.vst)); config_text.write ("\\n\\\n")
	config_text.write ('Wiimote Support '); config_text.write (str (opts.wiimote)); config_text.write ("\\n\\\n")
	config_text.write ('Windows Key '); config_text.write (str (opts.windows_key)); config_text.write ("\\n\\\n")
	config_text.write ('C Compiler flags '); config_text.write (str (conf.env['CCFLAGS'])); config_text.write ("\\n\\\n")
	config_text.write ('C++ Compiler flags '); config_text.write (str (conf.env['CXXFLAGS'])); config_text.write ("\\n\\\n")
	config_text.write ('";}\n')
	config_text.close ()

def build(bld):
	autowaf.set_recursive()
	if sys.platform == 'darwin':
		bld.add_subdirs('libs/appleutility')
	for i in children:
		bld.add_subdirs(i)

	# ideally, we'd like to use the OS-provided MIDI API
	# for default ports. that doesn't work on at least
	# Fedora (Nov 9th, 2009) so use JACK MIDI on linux.
	
	if sys.platform == 'darwin':
		rc_subst_dict = {
			'MIDITAG'    : 'control',
			'MIDITYPE'   : 'coremidi',
			'JACK_INPUT' : 'auditioner'
			}
	else:
		rc_subst_dict = {
			'MIDITAG'    : 'control',
			'MIDITYPE'   : 'jack',
			'JACK_INPUT' : 'auditioner'
			}

	obj              = bld.new_task_gen('subst')
	obj.source       = 'ardour.rc.in'
	obj.target       = 'ardour_system.rc'
	obj.dict         = rc_subst_dict
	obj.install_path = '${CONFIGDIR}/ardour3'

def i18n(bld):
	bld.recurse (i18n_children)
