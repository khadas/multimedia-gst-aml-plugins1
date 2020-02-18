#############################################################
#
# gat-aml-plugins1
#
#############################################################
GST_AML_PLUGINS1_VERSION = 1.0
#GST_AML_PLUGINS1_SITE =file://$(TOPDIR)/package/multimedia/gst-aml-plugins1/
GST_AML_PLUGINS1_SITE = $(TOPDIR)/../multimedia/gst-aml-plugins1/gst-aml-plugins-1.0
GST_AML_PLUGINS1_SITE_METHOD = local

GST_AML_PLUGINS1_INSTALL_STAGING = YES
GST_AML_PLUGINS1_AUTORECONF = YES
GST_AML_PLUGINS1_DEPENDENCIES = gstreamer1 host-pkgconf libplayer

ifeq ($(BR2_PACKAGE_GST_AML_PLUGINS1_AUDIO_HAL_SINK),y)
GST_AML_PLUGINS1_DEPENDENCIES += hal_audio_service
GST_AML_PLUGINS1_CONF_OPTS += --enable-aml-audio-hal-sink
endif

$(eval $(autotools-package))

