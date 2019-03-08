#include <sys/types.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/kernel.h>

static int dso100fb_loader(struct module *m, int what, void *arg) {
  (void) arg;

  switch(what) {
  case MOD_LOAD:
    printf("DSO100FB loaded\n");
    return 0;

  case MOD_UNLOAD:
    printf("DSO100FB unloaded\n");
    return 0;

  default:
    return EOPNOTSUPP;
  }
}

static moduledata_t dso100fb_mod = {
  "dso100fb",
  dso100fb_loader,
  NULL
};

DECLARE_MODULE(dso100fb, dso100fb_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
