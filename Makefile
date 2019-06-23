KMOD = dso100fb
SRCS = dso100fb.c device_if.h bus_if.h ofw_bus_if.h fb_if.h opt_syscons.h opt_teken.h opt_splash.h

.include <bsd.kmod.mk>
