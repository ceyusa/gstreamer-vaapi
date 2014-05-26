EXTRA_DIST =

videoutils_srcdir = \
	$(top_srcdir)/ext/videoutils/gst-libs/gst/video

videoutils_source_c = \
	gstvideodecoder.c	\
	gstvideoencoder.c	\
	gstvideoutils.c		\
	video.c			\
	$(NULL)

EXTRA_DIST += $(videoutils_source_c:%.c=$(videoutils_srcdir)/%.c)

videoutils_source_h = \
	gstvideodecoder.h	\
	gstvideoencoder.h	\
	gstvideoutils.h		\
	video.h			\
	$(NULL)

EXTRA_DIST += $(videoutils_source_h:%.h=$(videoutils_srcdir)/%.h)

# Extra clean files so that maintainer-clean removes *everything*
MAINTAINERCLEANFILES = Makefile.in
