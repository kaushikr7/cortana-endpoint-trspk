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
LINUX_VOICE_ASSISTANT_CPP_DEPENDENCIES = json-for-modern-cpp mpv webrtc-audio-processing libcurl alsa-lib

LINUX_VOICE_ASSISTANT_CPP_CONF_OPTS =

# Install the vendored UI sound effects (thinking/processing,
# timer ring, mute toggle). Originally lived in
# the Python LVA package's `sounds/` directory; ship our own copy so
# we don't depend on that package being selected.
define LINUX_VOICE_ASSISTANT_CPP_INSTALL_SOUNDS
	$(INSTALL) -d $(TARGET_DIR)/usr/share/thirdreality/sounds
	cp -f $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/sounds/*.wav \
	      $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/sounds/*.flac \
	      $(TARGET_DIR)/usr/share/thirdreality/sounds/
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_SOUNDS

$(eval $(cmake-package))
