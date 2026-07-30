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

# alsa-lib is used by the dev tool aec_loopback_test (and, in a follow-up
# change, will back the production AudioCapture's ALSA backend for
# hardware-loopback AEC). Keep it explicit even though pulseaudio /
# webrtc-audio-processing already pull it transitively.
LINUX_VOICE_ASSISTANT_CPP_DEPENDENCIES = json-for-modern-cpp pulseaudio webrtc-audio-processing libcurl alsa-lib

LINUX_VOICE_ASSISTANT_CPP_CONF_OPTS =

$(eval $(cmake-package))
