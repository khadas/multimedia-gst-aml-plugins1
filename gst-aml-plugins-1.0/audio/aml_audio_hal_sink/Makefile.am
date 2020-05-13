# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = libgstamlhalasink.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgstamlhalasink_la_SOURCES = gstamlhalasink.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstamlhalasink_la_CFLAGS = $(GST_CFLAGS)
libgstamlhalasink_la_LIBADD = $(GST_LIBS) -lgstaudio-1.0
libgstamlhalasink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstamlhalasink_la_LIBADD += -L$(TARGET_DIR)/usr/lib -laudio_client
libgstamlhalasink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
#noinst_HEADERS = 
