/*
 * Broadcom Northstar (BCM4708/4709/47094) SoC backplane devices:
 *   - ChipCommon-A register block (chip id, caps, EROM pointer, interrupt
 *     gating for the two CCA UARTs, 24 GPIOs, watchdog), with the board's
 *     buttons and LEDs as QOM properties
 *   - IDM/DMP wrapper pages (ioctrl / iostatus / resetctrl)
 *   - a register file with reset values, used for the DMU/CRU and the iHost
 *     clock-reset unit, whose only kernel-visible behaviour is "read back what
 *     CFE left there".
 *
 * Offsets: include/sbchipc.h, include/aidmp.h in the vendor GPL tree.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/bcm_ns.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "trace.h"

/* ======================================================================== */
/* ChipCommon-A                                                             */
/* ======================================================================== */

enum {
    CC_CHIPID           = 0x000,
    CC_CAPABILITIES     = 0x004,
    CC_CORECONTROL      = 0x008,
    CC_OTPSTATUS        = 0x010,
    CC_OTPLAYOUT        = 0x01c,
    CC_INTSTATUS        = 0x020,
    CC_INTMASK          = 0x024,
    CC_CHIPCONTROL      = 0x028,
    CC_CHIPSTATUS       = 0x02c,
    CC_GPIOPULLUP       = 0x058,
    CC_GPIOPULLDOWN     = 0x05c,
    CC_GPIOIN           = 0x060,
    CC_GPIOOUT          = 0x064,
    CC_GPIOOUTEN        = 0x068,
    CC_GPIOCONTROL      = 0x06c,
    CC_GPIOINTPOLARITY  = 0x070,
    CC_GPIOINTMASK      = 0x074,
    CC_GPIOEVENT        = 0x078,
    CC_GPIOEVENTINTMASK = 0x07c,
    CC_WATCHDOG         = 0x080,
    CC_GPIOEVENTINTPOL  = 0x084,
    CC_GPIOTIMERVAL     = 0x088,
    CC_GPIOTIMEROUTMASK = 0x08c,
    CC_CLKDIV           = 0x0a4,
    CC_CAPABILITIES_EXT = 0x0ac,
    CC_EROMPTR          = 0x0fc,
};

#define CI_GPIO     0x00000001
#define CI_UART     0x00000040
#define CI_WDRESET  0x80000000

#define GPIO_MASK   ((1u << BCM_NS_CC_NGPIO) - 1)

static uint32_t cc_gpioin(BCMNSChipCommonState *s)
{
    /* Output-enabled pins read back what we drive; inputs read the pad. */
    uint32_t pad = (s->gpio_ext_level & s->gpio_ext_driven) |
                   (s->gpio_pullup_in & ~s->gpio_ext_driven);
    return ((s->gpioout & s->gpioouten) | (pad & ~s->gpioouten)) & GPIO_MASK;
}

static void cc_update_irq(BCMNSChipCommonState *s)
{
    uint32_t in = cc_gpioin(s);
    uint32_t st = s->intstatus & ~(CI_GPIO | CI_UART);

    /* GPIO level interrupt: (in ^ polarity) & mask; edge events latched. */
    if (((in ^ s->gpiointpolarity) & s->gpiointmask) ||
        (s->gpioevent & s->gpioeventintmask)) {
        st |= CI_GPIO;
    }
    if (s->uart_irq_level) {
        st |= CI_UART;
    }
    s->intstatus = st;
    qemu_set_irq(s->irq, !!(st & s->intmask & ~CI_WDRESET));
}

static void cc_gpio_outputs(BCMNSChipCommonState *s, uint32_t old_level)
{
    uint32_t level = cc_gpioin(s);
    uint32_t changed = old_level ^ level;

    for (int i = 0; i < BCM_NS_CC_NGPIO; i++) {
        if (changed & (1u << i)) {
            trace_bcm_ns_gpio_level(i, (level >> i) & 1);
            qemu_set_irq(s->gpio_out[i], (level >> i) & 1);
        }
    }
}

static void cc_gpio_event(BCMNSChipCommonState *s, uint32_t old_in)
{
    uint32_t in = cc_gpioin(s);
    uint32_t rising = ~old_in & in, falling = old_in & ~in;

    /* gpioeventintpolarity bit set = falling edge */
    s->gpioevent |= (rising & ~s->gpioeventintpolarity) |
                    (falling & s->gpioeventintpolarity);
}

/*
 * LEDs: lit when the pin is an output driven to its active level. An LED on
 * an input pin is dark whatever the pad reads.
 */
static uint32_t cc_leds_lit(BCMNSChipCommonState *s)
{
    uint32_t on = 0;

    for (const BCMNSGpioPin *p = s->pins; p && p->name; p++) {
        if (p->kind == BCM_NS_PIN_LED && extract32(s->gpioouten, p->pin, 1) &&
            extract32(s->gpioout, p->pin, 1) != p->active_low) {
            on |= 1u << p->pin;
        }
    }
    return on;
}

static void cc_update_leds(BCMNSChipCommonState *s)
{
    uint32_t on = cc_leds_lit(s), changed = on ^ s->led_on;

    for (const BCMNSGpioPin *p = s->pins; changed && p && p->name; p++) {
        if (p->kind == BCM_NS_PIN_LED && (changed & (1u << p->pin))) {
            s->led_changes[p->pin]++;
            trace_bcm_ns_led(p->name, (on >> p->pin) & 1 ? "on" : "off");
        }
    }
    s->led_on = on;
}

/* Drive pin n externally to level (0/1), or release it (-1) to its pad level. */
static void cc_gpio_drive(BCMNSChipCommonState *s, int n, int level)
{
    uint32_t old = cc_gpioin(s);

    if (level < 0) {
        s->gpio_ext_driven &= ~(1u << n);
    } else {
        s->gpio_ext_driven |= 1u << n;
        s->gpio_ext_level = deposit32(s->gpio_ext_level, n, 1, !!level);
    }
    cc_gpio_event(s, old);
    cc_gpio_outputs(s, old);
    cc_update_irq(s);
}

/* External drive of a pin (switch SMI data line, ...). */
static void cc_gpio_set(void *opaque, int n, int level)
{
    cc_gpio_drive(opaque, n, !!level);
}

static void cc_uart_irq(void *opaque, int n, int level)
{
    BCMNSChipCommonState *s = opaque;

    s->uart_irq_level = deposit32(s->uart_irq_level, n, 1, !!level);
    cc_update_irq(s);
}

static void cc_wdt_expire(void *opaque)
{
    BCMNSChipCommonState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR, "bcm-ns-chipcommon: watchdog reset\n");
    s->intstatus |= CI_WDRESET;      /* survives the reset, see cc_reset */
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void cc_wdt_write(BCMNSChipCommonState *s, uint32_t ticks)
{
    s->watchdog = ticks;
    if (!ticks) {
        timer_del(s->wdt_timer);
        return;
    }
    timer_mod(s->wdt_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(ticks, NANOSECONDS_PER_SECOND, s->wdt_hz));
}

static uint64_t cc_read(void *opaque, hwaddr addr, unsigned size)
{
    BCMNSChipCommonState *s = opaque;

    switch (addr) {
    case CC_CHIPID:           return s->chipid;
    case CC_CAPABILITIES:     return s->capabilities;
    case CC_CORECONTROL:      return s->corecontrol;
    case CC_OTPSTATUS:        return s->otpstatus;
    case CC_OTPLAYOUT:        return s->otplayout;
    case CC_INTSTATUS:        return s->intstatus;
    case CC_INTMASK:          return s->intmask;
    case CC_CHIPCONTROL:      return s->chipcontrol;
    case CC_CHIPSTATUS:       return s->chipstatus;
    case CC_GPIOPULLUP:       return s->gpiopullup;
    case CC_GPIOPULLDOWN:     return s->gpiopulldown;
    case CC_GPIOIN:           return cc_gpioin(s);
    case CC_GPIOOUT:          return s->gpioout;
    case CC_GPIOOUTEN:        return s->gpioouten;
    case CC_GPIOCONTROL:      return s->gpiocontrol;
    case CC_GPIOINTPOLARITY:  return s->gpiointpolarity;
    case CC_GPIOINTMASK:      return s->gpiointmask;
    case CC_GPIOEVENT:        return s->gpioevent;
    case CC_GPIOEVENTINTMASK: return s->gpioeventintmask;
    case CC_WATCHDOG:         return s->watchdog;
    case CC_GPIOEVENTINTPOL:  return s->gpioeventintpolarity;
    case CC_GPIOTIMERVAL:     return s->gpiotimerval;
    case CC_GPIOTIMEROUTMASK: return s->gpiotimeroutmask;
    case CC_CLKDIV:           return s->clkdiv;
    case CC_CAPABILITIES_EXT: return s->capabilities_ext;
    case CC_EROMPTR:          return s->eromptr;
    default:
        qemu_log_mask(LOG_UNIMP, "bcm-ns-chipcommon: read 0x%03" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void cc_write(void *opaque, hwaddr addr, uint64_t val64, unsigned size)
{
    BCMNSChipCommonState *s = opaque;
    uint32_t val = val64, old_in = cc_gpioin(s);

    switch (addr) {
    case CC_CORECONTROL:      s->corecontrol = val; break;
    case CC_INTSTATUS:        s->intstatus &= ~(val & CI_WDRESET); break; /* W1C */
    case CC_INTMASK:          s->intmask = val; break;
    case CC_CHIPCONTROL:      s->chipcontrol = val; break;
    case CC_GPIOPULLUP:       s->gpiopullup = val & GPIO_MASK; break;
    case CC_GPIOPULLDOWN:     s->gpiopulldown = val & GPIO_MASK; break;
    case CC_GPIOOUT:          s->gpioout = val & GPIO_MASK; break;
    case CC_GPIOOUTEN:        s->gpioouten = val & GPIO_MASK; break;
    case CC_GPIOCONTROL:      s->gpiocontrol = val & GPIO_MASK; break;
    case CC_GPIOINTPOLARITY:  s->gpiointpolarity = val & GPIO_MASK; break;
    case CC_GPIOINTMASK:      s->gpiointmask = val & GPIO_MASK; break;
    case CC_GPIOEVENT:        s->gpioevent &= ~val; break;            /* W1C */
    case CC_GPIOEVENTINTMASK: s->gpioeventintmask = val & GPIO_MASK; break;
    case CC_WATCHDOG:         cc_wdt_write(s, val); break;
    case CC_GPIOEVENTINTPOL:  s->gpioeventintpolarity = val & GPIO_MASK; break;
    case CC_GPIOTIMERVAL:     s->gpiotimerval = val; break;
    case CC_GPIOTIMEROUTMASK: s->gpiotimeroutmask = val; break;
    case CC_CLKDIV:           s->clkdiv = val; break;
    case CC_CHIPID: case CC_CAPABILITIES: case CC_CHIPSTATUS:
    case CC_GPIOIN: case CC_CAPABILITIES_EXT: case CC_EROMPTR:
    case CC_OTPSTATUS: case CC_OTPLAYOUT:
        qemu_log_mask(LOG_GUEST_ERROR, "bcm-ns-chipcommon: write to RO 0x%03"
                      HWADDR_PRIx "\n", addr);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "bcm-ns-chipcommon: write 0x%03" HWADDR_PRIx
                      " = 0x%08x\n", addr, val);
        return;
    }
    cc_gpio_event(s, old_in);
    cc_gpio_outputs(s, old_in);
    cc_update_irq(s);
    cc_update_leds(s);
}

static const MemoryRegionOps cc_ops = {
    .read = cc_read,
    .write = cc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void cc_reset(DeviceState *dev)
{
    BCMNSChipCommonState *s = BCM_NS_CHIPCOMMON(dev);
    uint32_t wdreset = s->intstatus & CI_WDRESET;

    timer_del(s->wdt_timer);
    s->corecontrol = s->corecontrol_reset;
    s->intstatus = wdreset;          /* "watchdog reset occurred" is sticky */
    s->intmask = 0;
    s->chipcontrol = 0;
    s->gpiopullup = s->gpiopulldown = 0;
    s->gpioout = s->gpioouten = s->gpiocontrol = 0;
    s->gpiointpolarity = s->gpiointmask = 0;
    s->gpioevent = s->gpioeventintmask = s->gpioeventintpolarity = 0;
    s->gpiotimerval = s->gpiotimeroutmask = 0;
    s->watchdog = 0;
    s->clkdiv = s->clkdiv_reset;
    cc_update_leds(s);
}

/*
 * QOM interface for the pins (qom-get / qom-set on the ChipCommon device):
 *   gpio<N>        str: reads the level the guest sees, "0" or "1"; write
 *                  "0" or "1" to drive the pad, "z" to release it to its
 *                  board level. Driving an output pin changes nothing.
 *   button-<name>  bool: true holds the button (drives its active level),
 *                  false releases it.
 *   led-<name>     bool, read-only: the LED is lit.
 *   buttons, leds  str, read-only: "name:held|released ..." and
 *                  "name:on|off:<changes> ..." (changes since power-on).
 * LED changes are also trace events (bcm_ns_led), and every pin level
 * change is bcm_ns_gpio_level.
 */
static void cc_get_gpio(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    BCMNSChipCommonState *s = BCM_NS_CHIPCOMMON(obj);
    char *val = g_strdup(extract32(cc_gpioin(s), (uintptr_t)opaque, 1) ? "1" : "0");

    visit_type_str(v, name, &val, errp);
    g_free(val);
}

static void cc_set_gpio(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    BCMNSChipCommonState *s = BCM_NS_CHIPCOMMON(obj);
    g_autofree char *val = NULL;
    int level;

    if (!visit_type_str(v, name, &val, errp)) {
        return;
    }
    if (!strcmp(val, "0") || !strcmp(val, "1")) {
        level = val[0] - '0';
    } else if (!strcmp(val, "z")) {
        level = -1;
    } else {
        error_setg(errp, "%s: \"%s\" is not 0, 1 or z", name, val);
        return;
    }
    trace_bcm_ns_gpio_drive((uintptr_t)opaque, val);
    cc_gpio_drive(s, (uintptr_t)opaque, level);
}

static bool cc_button_held(BCMNSChipCommonState *s, const BCMNSGpioPin *p)
{
    return extract32(s->gpio_ext_driven, p->pin, 1) &&
           extract32(s->gpio_ext_level, p->pin, 1) != p->active_low;
}

static void cc_get_button(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    bool held = cc_button_held(BCM_NS_CHIPCOMMON(obj), opaque);

    visit_type_bool(v, name, &held, errp);
}

static void cc_set_button(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    const BCMNSGpioPin *p = opaque;
    bool held;

    if (!visit_type_bool(v, name, &held, errp)) {
        return;
    }
    trace_bcm_ns_button(p->name, held ? "held" : "released");
    cc_gpio_drive(BCM_NS_CHIPCOMMON(obj), p->pin, held ? !p->active_low : -1);
}

static void cc_get_led(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    const BCMNSGpioPin *p = opaque;
    bool lit = extract32(BCM_NS_CHIPCOMMON(obj)->led_on, p->pin, 1);

    visit_type_bool(v, name, &lit, errp);
}

static char *cc_summary(BCMNSChipCommonState *s, BCMNSPinKind kind)
{
    GString *out = g_string_new(NULL);

    for (const BCMNSGpioPin *p = s->pins; p && p->name; p++) {
        if (p->kind != kind) {
            continue;
        }
        if (out->len) {
            g_string_append_c(out, ' ');
        }
        if (kind == BCM_NS_PIN_BUTTON) {
            g_string_append_printf(out, "%s:%s", p->name,
                                   cc_button_held(s, p) ? "held" : "released");
        } else {
            g_string_append_printf(out, "%s:%s:%u", p->name,
                                   extract32(s->led_on, p->pin, 1) ? "on" : "off",
                                   s->led_changes[p->pin]);
        }
    }
    return g_string_free(out, false);
}

static char *cc_get_buttons(Object *obj, Error **errp)
{
    return cc_summary(BCM_NS_CHIPCOMMON(obj), BCM_NS_PIN_BUTTON);
}

static char *cc_get_leds(Object *obj, Error **errp)
{
    return cc_summary(BCM_NS_CHIPCOMMON(obj), BCM_NS_PIN_LED);
}

void bcm_ns_cc_set_pins(BCMNSChipCommonState *s, const BCMNSGpioPin *pins)
{
    Object *obj = OBJECT(s);

    assert(!s->pins);
    s->pins = pins;
    for (const BCMNSGpioPin *p = pins; p->name; p++) {
        g_autofree char *prop = g_strdup_printf("%s-%s",
            p->kind == BCM_NS_PIN_BUTTON ? "button" : "led", p->name);

        assert(p->pin < BCM_NS_CC_NGPIO);
        object_property_add(obj, prop, "bool",
                            p->kind == BCM_NS_PIN_BUTTON ? cc_get_button : cc_get_led,
                            p->kind == BCM_NS_PIN_BUTTON ? cc_set_button : NULL,
                            NULL, (void *)p);
    }
    object_property_add_str(obj, "buttons", cc_get_buttons, NULL);
    object_property_add_str(obj, "leds", cc_get_leds, NULL);
}

static void cc_realize(DeviceState *dev, Error **errp)
{
    BCMNSChipCommonState *s = BCM_NS_CHIPCOMMON(dev);

    if (!s->wdt_hz) {
        error_setg(errp, "watchdog-hz must be non-zero");
        return;
    }
    s->wdt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cc_wdt_expire, s);
}

static void cc_init(Object *obj)
{
    BCMNSChipCommonState *s = BCM_NS_CHIPCOMMON(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &cc_ops, s, "bcm-ns.chipcommon", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), cc_uart_irq, "uart-irq", 2);
    qdev_init_gpio_in_named(DEVICE(obj), cc_gpio_set, "gpio-in", BCM_NS_CC_NGPIO);
    qdev_init_gpio_out_named(DEVICE(obj), s->gpio_out, "gpio-out", BCM_NS_CC_NGPIO);
    for (uintptr_t n = 0; n < BCM_NS_CC_NGPIO; n++) {
        g_autofree char *prop = g_strdup_printf("gpio%u", (unsigned)n);

        object_property_add(obj, prop, "str", cc_get_gpio, cc_set_gpio, NULL, (void *)n);
    }
}

static const Property cc_properties[] = {
    DEFINE_PROP_UINT32("chipid", BCMNSChipCommonState, chipid, 0),
    DEFINE_PROP_UINT32("capabilities", BCMNSChipCommonState, capabilities, 0),
    DEFINE_PROP_UINT32("capabilities-ext", BCMNSChipCommonState, capabilities_ext, 0),
    DEFINE_PROP_UINT32("chipstatus", BCMNSChipCommonState, chipstatus, 0),
    DEFINE_PROP_UINT32("otpstatus", BCMNSChipCommonState, otpstatus, 0),
    DEFINE_PROP_UINT32("otplayout", BCMNSChipCommonState, otplayout, 0),
    DEFINE_PROP_UINT32("eromptr", BCMNSChipCommonState, eromptr, 0),
    DEFINE_PROP_UINT32("gpio-pullup-in", BCMNSChipCommonState, gpio_pullup_in, GPIO_MASK),
    DEFINE_PROP_UINT32("corecontrol", BCMNSChipCommonState, corecontrol_reset, 0),
    DEFINE_PROP_UINT32("clkdiv", BCMNSChipCommonState, clkdiv_reset, 0),
    DEFINE_PROP_UINT32("watchdog-hz", BCMNSChipCommonState, wdt_hz, 125000000),
};

static int cc_post_load(void *opaque, int version_id)
{
    BCMNSChipCommonState *s = opaque;

    s->led_on = cc_leds_lit(s);         /* derived; the change counts restart */
    return 0;
}

static const VMStateDescription vmstate_cc = {
    .name = TYPE_BCM_NS_CHIPCOMMON,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = cc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(corecontrol, BCMNSChipCommonState),
        VMSTATE_UINT32(intstatus, BCMNSChipCommonState),
        VMSTATE_UINT32(intmask, BCMNSChipCommonState),
        VMSTATE_UINT32(chipcontrol, BCMNSChipCommonState),
        VMSTATE_UINT32(gpiopullup, BCMNSChipCommonState),
        VMSTATE_UINT32(gpiopulldown, BCMNSChipCommonState),
        VMSTATE_UINT32(gpioout, BCMNSChipCommonState),
        VMSTATE_UINT32(gpioouten, BCMNSChipCommonState),
        VMSTATE_UINT32(gpiocontrol, BCMNSChipCommonState),
        VMSTATE_UINT32(gpiointpolarity, BCMNSChipCommonState),
        VMSTATE_UINT32(gpiointmask, BCMNSChipCommonState),
        VMSTATE_UINT32(gpioevent, BCMNSChipCommonState),
        VMSTATE_UINT32(gpioeventintmask, BCMNSChipCommonState),
        VMSTATE_UINT32(gpioeventintpolarity, BCMNSChipCommonState),
        VMSTATE_UINT32(gpiotimerval, BCMNSChipCommonState),
        VMSTATE_UINT32(gpiotimeroutmask, BCMNSChipCommonState),
        VMSTATE_UINT32(gpio_ext_level, BCMNSChipCommonState),
        VMSTATE_UINT32(gpio_ext_driven, BCMNSChipCommonState),
        VMSTATE_UINT32(watchdog, BCMNSChipCommonState),
        VMSTATE_UINT32(clkdiv, BCMNSChipCommonState),
        VMSTATE_UINT32(uart_irq_level, BCMNSChipCommonState),
        VMSTATE_TIMER_PTR(wdt_timer, BCMNSChipCommonState),
        VMSTATE_END_OF_LIST()
    },
};

static void cc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = cc_realize;
    dc->vmsd = &vmstate_cc;
    device_class_set_legacy_reset(dc, cc_reset);
    device_class_set_props(dc, cc_properties);
}

/* ======================================================================== */
/* IDM (DMP) wrappers                                                       */
/* ======================================================================== */

enum {
    IDM_OOBSELOUTA30 = 0x100,
    IDM_IOCTRLSET    = 0x400,
    IDM_IOCTRLCLEAR  = 0x404,
    IDM_IOCTRL       = 0x408,
    IDM_IOSTATUS     = 0x500,
    IDM_RESETCTRL    = 0x800,
    IDM_RESETSTATUS  = 0x804,
};

static int idm_page(hwaddr wrapper_pa)
{
    hwaddr off = wrapper_pa - BCM_NS_IDM_BASE;

    assert(wrapper_pa >= BCM_NS_IDM_BASE && (off >> 12) < BCM_NS_IDM_PAGES);
    return off >> 12;
}

void bcm_ns_idm_set_iostatus(BCMNSIDMState *s, hwaddr wrapper_pa, uint32_t val)
{
    s->iostatus[idm_page(wrapper_pa)] = val;
}

/* ioctrl as the boot loader leaves it, restored on every reset */
void bcm_ns_idm_set_ioctrl(BCMNSIDMState *s, hwaddr wrapper_pa, uint32_t val)
{
    s->ioctrl_reset[idm_page(wrapper_pa)] = val;
    s->ioctrl[idm_page(wrapper_pa)] = val;
}

void bcm_ns_idm_set_oobsel(BCMNSIDMState *s, hwaddr wrapper_pa, uint32_t val)
{
    s->oobselouta30[idm_page(wrapper_pa)] = val;
}

static uint64_t idm_read(void *opaque, hwaddr addr, unsigned size)
{
    BCMNSIDMState *s = opaque;
    int p = addr >> 12;

    switch (addr & 0xfff) {
    case IDM_OOBSELOUTA30: return s->oobselouta30[p];
    case IDM_IOCTRL:       return s->ioctrl[p];
    case IDM_IOSTATUS:     return s->iostatus[p];
    case IDM_RESETCTRL:    return s->resetctrl[p];
    case IDM_RESETSTATUS:  return 0;    /* reset completes immediately */
    default:               return 0;
    }
}

static void idm_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BCMNSIDMState *s = opaque;
    int p = addr >> 12;

    switch (addr & 0xfff) {
    case IDM_IOCTRLSET:   s->ioctrl[p] |= val; break;
    case IDM_IOCTRLCLEAR: s->ioctrl[p] &= ~val; break;
    case IDM_IOCTRL:      s->ioctrl[p] = val; break;
    case IDM_RESETCTRL:   s->resetctrl[p] = val & 1; break;
    default:              break;
    }
}

static const MemoryRegionOps idm_ops = {
    .read = idm_read,
    .write = idm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void idm_reset(DeviceState *dev)
{
    BCMNSIDMState *s = BCM_NS_IDM(dev);

    memcpy(s->ioctrl, s->ioctrl_reset, sizeof(s->ioctrl));
    memset(s->resetctrl, 0, sizeof(s->resetctrl));
}

static void idm_init(Object *obj)
{
    BCMNSIDMState *s = BCM_NS_IDM(obj);

    memory_region_init_io(&s->iomem, obj, &idm_ops, s, "bcm-ns.idm",
                          BCM_NS_IDM_PAGES * 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_idm = {
    .name = TYPE_BCM_NS_IDM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ioctrl, BCMNSIDMState, BCM_NS_IDM_PAGES),
        VMSTATE_UINT32_ARRAY(resetctrl, BCMNSIDMState, BCM_NS_IDM_PAGES),
        VMSTATE_END_OF_LIST()
    },
};

static void idm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_idm;
    device_class_set_legacy_reset(dc, idm_reset);
}

/* ======================================================================== */
/* Register file with reset values                                          */
/* ======================================================================== */

static uint64_t regfile_read(void *opaque, hwaddr addr, unsigned size)
{
    BCMNSRegFileState *s = opaque;

    return s->regs[addr >> 2];
}

static void regfile_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BCMNSRegFileState *s = opaque;
    uint32_t i = addr >> 2;

    s->regs[i] = (s->regs[i] & s->ro[i]) | (val & ~s->ro[i]);
}

static const MemoryRegionOps regfile_ops = {
    .read = regfile_read,
    .write = regfile_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void regfile_reset(DeviceState *dev)
{
    BCMNSRegFileState *s = BCM_NS_REGFILE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ro, 0, sizeof(s->ro));
    for (const BCMNSRegInit *r = s->init; r && r->offset != UINT32_MAX; r++) {
        s->regs[r->offset >> 2] = r->value;
        s->ro[r->offset >> 2] = r->ro_mask;
    }
}

static void regfile_realize(DeviceState *dev, Error **errp)
{
    BCMNSRegFileState *s = BCM_NS_REGFILE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &regfile_ops, s,
                          s->name ? s->name : "bcm-ns.regfile", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property regfile_properties[] = {
    DEFINE_PROP_STRING("name", BCMNSRegFileState, name),
};

static const VMStateDescription vmstate_regfile = {
    .name = TYPE_BCM_NS_REGFILE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCMNSRegFileState, BCM_NS_REGFILE_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void regfile_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = regfile_realize;
    dc->vmsd = &vmstate_regfile;
    device_class_set_legacy_reset(dc, regfile_reset);
    device_class_set_props(dc, regfile_properties);
}

BCMNSRegFileState *bcm_ns_regfile_create(const char *name, hwaddr base,
                                         const BCMNSRegInit *init)
{
    DeviceState *dev = qdev_new(TYPE_BCM_NS_REGFILE);
    BCMNSRegFileState *s = BCM_NS_REGFILE(dev);

    qdev_prop_set_string(dev, "name", name);
    s->init = init;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return s;
}

static const TypeInfo bcm_ns_types[] = {
    {
        .name          = TYPE_BCM_NS_CHIPCOMMON,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCMNSChipCommonState),
        .instance_init = cc_init,
        .class_init    = cc_class_init,
    }, {
        .name          = TYPE_BCM_NS_IDM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCMNSIDMState),
        .instance_init = idm_init,
        .class_init    = idm_class_init,
    }, {
        .name          = TYPE_BCM_NS_REGFILE,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCMNSRegFileState),
        .class_init    = regfile_class_init,
    },
};

DEFINE_TYPES(bcm_ns_types)
