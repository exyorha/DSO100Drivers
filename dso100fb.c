#include <sys/cdefs.h>

#include <sys/types.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/fbio.h>
#include <sys/consio.h>

#include <machine/bus.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/vt/vt.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include "fb_if.h"

#include "DSO100FB.h"

struct dso100fb_panel_config {
  uint32_t width;
  uint32_t hfrontporch;
  uint32_t hsync;
  uint32_t hbackporch;
  uint32_t overlayx;
  uint32_t overlaywidth;
  uint32_t height;
  uint32_t vfrontporch;
  uint32_t vsync;
  uint32_t vbackporch;
  uint32_t overlayy;
  uint32_t overlayheight;
  uint32_t deinverted;
  uint32_t hsyncinverted;
  uint32_t vsyncinverted;
};

struct dso100fb_panel_config_entry {
  const char *name;
  size_t offset;
};

static const struct dso100fb_panel_config_entry dso100fb_panel_config_entries[] = {
#define DSO100_PANEL_CONFIG_ENTRY(name) { #name, offsetof(struct dso100fb_panel_config, name) }
  DSO100_PANEL_CONFIG_ENTRY(width),
  DSO100_PANEL_CONFIG_ENTRY(hfrontporch),
  DSO100_PANEL_CONFIG_ENTRY(hsync),
  DSO100_PANEL_CONFIG_ENTRY(hbackporch),
  DSO100_PANEL_CONFIG_ENTRY(overlayx),
  DSO100_PANEL_CONFIG_ENTRY(overlaywidth),
  DSO100_PANEL_CONFIG_ENTRY(height),
  DSO100_PANEL_CONFIG_ENTRY(vfrontporch),
  DSO100_PANEL_CONFIG_ENTRY(vsync),
  DSO100_PANEL_CONFIG_ENTRY(vbackporch),
  DSO100_PANEL_CONFIG_ENTRY(overlayy),
  DSO100_PANEL_CONFIG_ENTRY(overlayheight),
  DSO100_PANEL_CONFIG_ENTRY(deinverted),
  DSO100_PANEL_CONFIG_ENTRY(hsyncinverted),
  DSO100_PANEL_CONFIG_ENTRY(vsyncinverted)
#undef DSO100_PANEL_CONFIG_ENTRY
};

struct dso100fb_softc {
  struct fb_info fb_info;
  struct mtx intr_mtx;
  struct cv intr_cv;
  uint32_t intr_mask;
  device_t dev;
  struct resource *mem_res;
  int mem_rid;
  struct resource *irq_res;
  int irq_rid;
  void *intr_cookie;
  bus_dma_tag_t dma_tag;
  void *fb_base;
  size_t fb_size;
  bus_addr_t fb_phys;
  device_t fbd;
};

static void dso100fb_intr(void *arg) {
  struct dso100fb_softc *softc = arg;
  uint32_t status;

  status = bus_read_4(
    softc->mem_res,
    DSO100FB_REG_ISR
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_ISR,
    ~status
  );

  mtx_lock(&softc->intr_mtx);

  softc->intr_mask |= status;

  cv_broadcast(&softc->intr_cv);

  mtx_unlock(&softc->intr_mtx);
}

static int dso100fb_probe(device_t dev) {
  if(!ofw_bus_status_okay(dev) ||
     !ofw_bus_is_compatible(dev, "dso100,dso100fb"))
    return ENXIO;

  device_set_desc(dev, "DSO-100 framebuffer controller");

  return BUS_PROBE_DEFAULT;
}

static int dso100fb_read_panel_config(
  device_t dev,
  struct dso100fb_panel_config *panel_config) {

  phandle_t node = ofw_bus_get_node(dev);
  size_t index;
  const char *name;
  uint32_t *ptr;
  int result;

  for(index = 0; index < sizeof(dso100fb_panel_config_entries) / sizeof(dso100fb_panel_config_entries[0]); index++) {
    name = dso100fb_panel_config_entries[index].name;
    ptr = (uint32_t *)((uint8_t *)panel_config + dso100fb_panel_config_entries[index].offset);
    result = OF_getencprop(node, name, ptr, sizeof(*ptr));
    if(result < 0) {
      device_printf(dev, "required parameter %s is not specified in OF\n", name);
      return -ENXIO;
    }
  }

  return 0;
}

static void dso100fb_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int err) {
  bus_addr_t *addr = arg;

  if(err == 0) {
      addr[0] = segs[0].ds_addr;
  }
}

static void dso100fb_signal_and_wait_for_interrupts(
  struct dso100fb_softc *softc,
  uint32_t signal,
  uint32_t mask
) {

  mtx_lock(&softc->intr_mtx);

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_IMR,
    mask
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_CR,
    signal
  );

  while((softc->intr_mask & mask) == 0) {
    cv_wait(&softc->intr_cv, &softc->intr_mtx);
  }

  softc->intr_mask = 0;

  bus_write_4(softc->mem_res,
    DSO100FB_REG_IMR,
    0
  );

  mtx_unlock(&softc->intr_mtx);
}

static int dso100fb_configure(
  struct dso100fb_softc *softc,
  const struct dso100fb_panel_config *cfg) {

  int err = 0;
  uint32_t ifctrl = 0;
  size_t framebuffer_size;

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_HTIMING1,
    ((cfg->overlayx << DSO100FB_HTIMING1_WIDTHBEFOREOVERLAY_POS) & DSO100FB_HTIMING1_WIDTHBEFOREOVERLAY_MASK) |
    ((cfg->overlaywidth << DSO100FB_HTIMING1_WIDTHOVERLAY_POS) & DSO100FB_HTIMING1_WIDTHOVERLAY_MASK)
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_HTIMING2,
    (((cfg->width - cfg->overlaywidth - cfg->overlayx) << DSO100FB_HTIMING2_WIDTHAFTEROVERLAY_POS) & DSO100FB_HTIMING2_WIDTHAFTEROVERLAY_MASK) |
    ((cfg->hfrontporch << DSO100FB_HTIMING2_FRONTPORCH_POS) & DSO100FB_HTIMING2_FRONTPORCH_MASK)
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_HTIMING3,
    ((cfg->hsync << DSO100FB_HTIMING3_SYNCPULSE_POS) & DSO100FB_HTIMING3_SYNCPULSE_MASK) |
    ((cfg->hbackporch << DSO100FB_HTIMING3_BACKPORCH_POS) & DSO100FB_HTIMING3_BACKPORCH_MASK)
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_VTIMING1,
    ((cfg->overlayy << DSO100FB_VTIMING1_HEIGHTBEFOREOVERLAY_POS) & DSO100FB_VTIMING1_HEIGHTBEFOREOVERLAY_MASK) |
    ((cfg->overlayheight << DSO100FB_VTIMING1_HEIGHTOVERLAY_POS) & DSO100FB_VTIMING1_HEIGHTOVERLAY_MASK)
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_VTIMING2,
    (((cfg->height - cfg->overlayheight - cfg->overlayy) << DSO100FB_VTIMING2_HEIGHTAFTEROVERLAY_POS) & DSO100FB_VTIMING2_HEIGHTAFTEROVERLAY_MASK) |
    ((cfg->vfrontporch << DSO100FB_VTIMING2_FRONTPORCH_POS) & DSO100FB_VTIMING2_FRONTPORCH_MASK)
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_VTIMING3,
    ((cfg->vsync << DSO100FB_VTIMING3_SYNCPULSE_POS) & DSO100FB_VTIMING3_SYNCPULSE_MASK) |
    ((cfg->vbackporch << DSO100FB_VTIMING3_BACKPORCH_POS) & DSO100FB_VTIMING3_BACKPORCH_MASK)
  );

  if(cfg->deinverted)
    ifctrl |= DSO100FB_IFCTRL_DE_POL;

  if(cfg->hsyncinverted)
    ifctrl |= DSO100FB_IFCTRL_HSYNC_POL;

  if(cfg->vsyncinverted)
    ifctrl |= DSO100FB_IFCTRL_VSYNC_POL;

  bus_write_4(softc->mem_res, DSO100FB_REG_IFCTRL, ifctrl);

  softc->fb_info.fb_name = device_get_nameunit(softc->dev);
  softc->fb_info.fb_width = cfg->width;
  softc->fb_info.fb_height = cfg->height;
  softc->fb_info.fb_depth = 32;
  softc->fb_info.fb_bpp = 32;
  softc->fb_info.fb_stride = cfg->width * (softc->fb_info.fb_bpp / 8);
  softc->fb_info.fb_size = softc->fb_info.fb_stride * softc->fb_info.fb_height;
  softc->fb_info.fb_flags = FB_FLAG_MEMATTR;
  softc->fb_info.fb_memattr = VM_MEMATTR_WRITE_COMBINING;
  framebuffer_size = round_page(softc->fb_info.fb_size);
  softc->fb_size = framebuffer_size;

  softc->fb_base = (void *)kmem_alloc_contig(framebuffer_size, M_NOWAIT | M_ZERO, 0, ~0, 4096, 0, VM_MEMATTR_WRITE_COMBINING);
  if(softc->fb_base == NULL) {
    return ENOMEM;
  }

  softc->fb_phys = pmap_kextract((uintptr_t)softc->fb_base);

  softc->fb_info.fb_vbase = (uintptr_t)softc->fb_base;
  softc->fb_info.fb_pbase = softc->fb_phys;

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_FB_BASE,
    softc->fb_phys
  );

  bus_write_4(
    softc->mem_res,
    DSO100FB_REG_FB_LENGTH,
    softc->fb_info.fb_size
  );

  softc->fbd = device_add_child(softc->dev, "fbd",
    device_get_unit(softc->dev));

  if(softc->fbd == NULL) {
    err = ENOMEM;
    goto free_fb;
  }

  err = device_probe_and_attach(softc->fbd);
  if(err != 0) {
    goto release_fbd;
	}

  dso100fb_signal_and_wait_for_interrupts(
    softc,
    DSO100FB_CR_START,
    DSO100FB_ISR_STARTED
  );

  return 0;

release_fbd:
  device_delete_child(softc->dev, softc->fbd);

free_fb:
  kmem_free(softc->fb_info.fb_vbase, softc->fb_size);

  return err;
}

static int dso100fb_attach(device_t dev) {
  struct dso100fb_softc *softc = device_get_softc(dev);
  int err;
  struct dso100fb_panel_config panel_config;

  mtx_init(&softc->intr_mtx, "dso100fb intr_mtx", NULL, MTX_DEF);
  cv_init(&softc->intr_cv, "dso100fb intr_cv");
  softc->intr_mask = 0;

  softc->dev = dev;

  softc->mem_rid = 0;
  softc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &softc->mem_rid,
    RF_ACTIVE);
  if(!softc->mem_res) {
    device_printf(dev, "cannot allocate memory\n");
    return ENXIO;
  }

  softc->irq_rid = 0;
  softc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &softc->irq_rid,
    RF_ACTIVE);
  if(!softc->irq_res) {
    device_printf(dev, "cannot allocate IRQ\n");
    err = ENXIO;
    goto release_memory_res;
  }

  err = bus_setup_intr(
    dev, softc->irq_res, INTR_TYPE_MISC | INTR_MPSAFE, NULL, dso100fb_intr,
    softc, &softc->intr_cookie
  );

  if(err != 0) {
    device_printf(dev, "cannot setup interrupt: %d", err);
    goto release_interrupt_res;
  }

  err = dso100fb_read_panel_config(dev, &panel_config);
  if(err != 0) {
    goto teardown_intr;
  }

  err = dso100fb_configure(softc, &panel_config);

  if(err == 0)
    return 0;

teardown_intr:
  bus_teardown_intr(dev, softc->irq_res, softc->intr_cookie);

release_interrupt_res:
  bus_release_resource(dev, SYS_RES_IRQ, softc->irq_rid, softc->irq_res);

release_memory_res:
  bus_release_resource(dev, SYS_RES_MEMORY, softc->mem_rid, softc->mem_res);

  return err;
}

static int dso100fb_detach(device_t dev) {
  struct dso100fb_softc *softc = device_get_softc(dev);

  dso100fb_signal_and_wait_for_interrupts(
    softc,
    DSO100FB_CR_STOP,
    DSO100FB_ISR_STOPPED
  );

  device_delete_child(softc->dev, softc->fbd);
  kmem_free(softc->fb_info.fb_vbase, softc->fb_size);
  bus_teardown_intr(dev, softc->irq_res, softc->intr_cookie);
  bus_release_resource(dev, SYS_RES_IRQ, softc->irq_rid, softc->irq_res);
  bus_release_resource(dev, SYS_RES_MEMORY, softc->mem_rid, softc->mem_res);

  cv_destroy(&softc->intr_cv);
  mtx_destroy(&softc->intr_mtx);

  return 0;
}

static struct fb_info *dso100fb_getinfo(device_t dev) {
  struct dso100fb_softc *softc = device_get_softc(dev);

  return &softc->fb_info;
}

static device_method_t dso100fb_methods[] = {
  DEVMETHOD(device_probe, dso100fb_probe),
  DEVMETHOD(device_attach, dso100fb_attach),
  DEVMETHOD(device_detach, dso100fb_detach),
  DEVMETHOD(fb_getinfo, dso100fb_getinfo),
  DEVMETHOD_END
};

static driver_t dso100fb_driver = {
  "fb",
  dso100fb_methods,
  sizeof(struct dso100fb_softc)
};

static devclass_t dso100fb_devclass;

DRIVER_MODULE(dso100fb, simplebus, dso100fb_driver, dso100fb_devclass, NULL, NULL);
