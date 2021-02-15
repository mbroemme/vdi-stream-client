# the main programs.
bin_PROGRAMS			= vdi-stream-client

# sources for vdi-stream-client program.
vdi_stream_client_SOURCES	= vdi-stream-client.c engine-parsec.c
vdi_stream_client_CFLAGS	= @LIBSDL2_CFLAGS@
vdi_stream_client_LDADD		= @LIBSDL2_LIBS@

# install libparsec for dso loading.
if INTERNAL_PARSEC_SDK
vdi_stream_client_LDFLAGS	= -Wl,-rpath,$(libdir)/vdi-stream-client/plugins
libparsecdir			= $(libdir)/vdi-stream-client/plugins
libparsec_DATA			= ../parsec-sdk/sdk/linux/libparsec.so
EXTRA_DIST			= $(libparsec_DATA)
endif
