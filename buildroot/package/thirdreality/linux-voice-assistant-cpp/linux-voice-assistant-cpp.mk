################################################################################
#
# linux-voice-assistant-cpp
#
# Native Cortana voice endpoint for the ThirdReality speaker.
#
################################################################################

LINUX_VOICE_ASSISTANT_CPP_VERSION = 1.0.0
LINUX_VOICE_ASSISTANT_CPP_SITE = $(TOPDIR)/package/thirdreality/linux-voice-assistant-cpp
LINUX_VOICE_ASSISTANT_CPP_SITE_METHOD = local
LINUX_VOICE_ASSISTANT_CPP_LICENSE = Apache-2.0
LINUX_VOICE_ASSISTANT_CPP_LICENSE_FILES =

# alsa-lib backs production capture through the PulseAudio ALSA plugin and is
# also used directly by the retained aec_loopback_test development diagnostic.
LINUX_VOICE_ASSISTANT_CPP_DEPENDENCIES = json-for-modern-cpp pulseaudio webrtc-audio-processing libcurl alsa-lib

LINUX_VOICE_ASSISTANT_CPP_CONF_OPTS =

$(eval $(cmake-package))
