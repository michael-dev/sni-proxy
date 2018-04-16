bin_PROGRAMS=sni-proxy
sni_proxy_SOURCES=	sni-proxy.c \
					getaddr.c	\
					util.c	\
					listener.c \
					ringbuf.c \
					proxy.c

sni_proxy_LDADD=	$(top_builddir)/ucl/src/libucl.la -lanl
sni_proxy_CFLAGS=	-I$(top_srcdir)/ucl/include
