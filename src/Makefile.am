# the main programs.
bin_PROGRAMS			= vdi-stream-client

# sources for vdi-stream-client program.
vdi_stream_client_SOURCES	= client.c parsec.c redirect.c audio.c video.c
vdi_stream_client_CFLAGS	= $(USB_CFLAGS) $(USBREDIRHOST_CFLAGS) $(USBREDIRPARSER_CFLAGS) $(X11_CFLAGS) $(GL_CFLAGS) $(SDL2_CFLAGS) $(SDL2_TTF_CFLAGS)
vdi_stream_client_LDADD		= $(USB_LIBS) $(USBREDIRHOST_LIBS) $(USBREDIRPARSER_LIBS) $(X11_LIBS) $(GL_LIBS) $(SDL2_LIBS) $(SDL2_TTF_LIBS)

# install libparsec for dso loading.
if INTERNAL_PARSEC_SDK
vdi_stream_client_LDFLAGS	= -Wl,-rpath,$(libdir)/vdi-stream-client/plugins
libparsecdir			= $(libdir)/vdi-stream-client/plugins
libparsec_DATA			= ../parsec-sdk/sdk/linux/libparsec.so
EXTRA_DIST			= $(libparsec_DATA)
endif
