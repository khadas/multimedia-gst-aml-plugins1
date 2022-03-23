# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
lib_LIBRARIES = libcommon.a

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libcommon_a_SOURCES = $(top_srcdir)/common/amlsysctl/gstamlsysctl.c $(top_srcdir)/common/amlsysctl/gstamlsysctl.h $(top_srcdir)/common/amstreaminfo/amlstreaminfo.c $(top_srcdir)/common/amstreaminfo/amlstreaminfo.h $(top_srcdir)/common/amstreaminfo/amlutils.c $(top_srcdir)/common/amstreaminfo/amlutils.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libcommon_a_CFLAGS = $(GST_CFLAGS) -fPIC
noinst_HEADERS = $(top_srcdir)/common/amlsysctl/gstamlsysctl.h $(top_srcdir)/common/amstreaminfo/amlstreaminfo.h $(top_srcdir)/common/amstreaminfo/amlutils.h
