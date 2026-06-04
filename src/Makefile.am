# the main programs.
bin_PROGRAMS			= vdi-stream-client

# sources for vdi-stream-client program.
vdi_stream_client_SOURCES	= client.c parsec.c ffmpeg.c redirect.c audio.c video.c input.c
vdi_stream_client_CFLAGS	= $(USB_CFLAGS) $(USBREDIRHOST_CFLAGS) $(USBREDIRPARSER_CFLAGS) $(SDL3_CFLAGS) $(SDL3_TTF_CFLAGS) $(FFMPEG_CFLAGS)
vdi_stream_client_LDADD		= $(USB_LIBS) $(USBREDIRHOST_LIBS) $(USBREDIRPARSER_LIBS) $(SDL3_LIBS) $(SDL3_TTF_LIBS) $(FFMPEG_LIBS)

# install libparsec for dso loading. Redistribution requires Parsec SDK license permission.
if INTERNAL_PARSEC_SDK
vdi_stream_client_LDFLAGS	= -Wl,-rpath,$(libdir)/vdi-stream-client/plugins
libparsecdir			= $(libdir)/vdi-stream-client/plugins
libparsec_DATA			= ../parsec-sdk/sdk/linux/libparsec.so
EXTRA_DIST			= $(libparsec_DATA)
endif
