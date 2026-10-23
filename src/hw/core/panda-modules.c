/*
 * Panda modules: external boards and devices loaded as shared objects
 * (-M PATH[:NAME], -module PATH). The loading and the ABI check are
 * util/module.c's module_load_path(); this file keeps which QOM types came
 * from which module, resolves -M PATH[:NAME], and checks module devices
 * for vmstate. All of it is startup or snapshot time, and none of it runs
 * unless a module is named.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/target-info.h"
#include "qom/object.h"
#include "hw/core/qdev.h"
#include "hw/core/boards.h"
#include "hw/core/panda-modules.h"

/* typename -> path of the module that registered it */
static GHashTable *module_types;
/* realpath -> GPtrArray of its type names */
static GHashTable *modules;

static void collect_type(ObjectClass *oc, void *opaque)
{
    g_hash_table_add(opaque, (gpointer)object_class_get_name(oc));
}

static GHashTable *all_types(void)
{
    GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);

    object_class_foreach(collect_type, NULL, true, set);
    return set;
}

GPtrArray *panda_module_load(const char *path, Error **errp)
{
    g_autofree char *real = realpath(path, NULL);
    g_autoptr(GHashTable) before = NULL;
    g_autoptr(GHashTable) after = NULL;
    GPtrArray *types;
    GHashTableIter it;
    gpointer name;

    if (phase_check(PHASE_MACHINE_CREATED)) {
        error_setg(errp, "%s: modules load only before the machine is "
                   "created", path);
        return NULL;
    }
    if (!real) {
        error_setg_errno(errp, errno, "%s", path);
        return NULL;
    }
    if (!modules) {
        modules = g_hash_table_new(g_str_hash, g_str_equal);
        module_types = g_hash_table_new(g_str_hash, g_str_equal);
    }
    types = g_hash_table_lookup(modules, real);
    if (types) {
        return g_ptr_array_ref(types);
    }

    before = all_types();
    if (!module_load_path(real, errp)) {
        return NULL;
    }
    after = all_types();

    types = g_ptr_array_new();
    g_hash_table_iter_init(&it, after);
    while (g_hash_table_iter_next(&it, &name, NULL)) {
        if (!g_hash_table_contains(before, name)) {
            g_ptr_array_add(types, name);
            g_hash_table_insert(module_types, name, g_strdup(path));
        }
    }
    g_hash_table_insert(modules, g_steal_pointer(&real), types);
    return g_ptr_array_ref(types);
}

const char *panda_module_type_origin(const char *typename)
{
    return module_types ? g_hash_table_lookup(module_types, typename) : NULL;
}

char *panda_module_machine_spec(const char *spec, Error **errp)
{
    g_autofree char *path = g_strdup(spec);
    g_autoptr(GPtrArray) types = NULL;
    g_autoptr(GString) names = g_string_new("");
    char *want = strrchr(path, ':');
    const char *name = NULL;
    int n = 0;

    if (!strchr(spec, '/') && !g_str_has_suffix(spec, CONFIG_HOST_DSOSUF) &&
        !(want && strstr(spec, CONFIG_HOST_DSOSUF ":"))) {
        return NULL;
    }
    if (want && !strchr(want, '/')) {
        *want++ = '\0';
    } else {
        want = NULL;
    }
    types = panda_module_load(path, errp);
    if (!types) {
        return NULL;
    }
    for (guint i = 0; i < types->len; i++) {
        ObjectClass *oc = object_class_by_name(types->pdata[i]);
        MachineClass *mc;

        /* A machine type of this target (it implements the target's interface). */
        if (!object_class_dynamic_cast(oc, TYPE_MACHINE) ||
            !object_class_dynamic_cast(oc, target_machine_typename()) ||
            object_class_is_abstract(oc)) {
            continue;
        }
        mc = MACHINE_CLASS(oc);
        g_string_append_printf(names, " %s", mc->name);
        if (!want || g_str_equal(want, mc->name)) {
            name = mc->name;
            n++;
        }
    }
    if (n != 1) {
        error_setg(errp, "%s: %s; it registered:%s", path,
                   want ? "no such machine type" : "pick one with PATH:NAME",
                   names->len ? names->str : " no machine types");
        return NULL;
    }
    return g_strdup(name);
}

typedef struct VmstateCheck {
    GHashTable *warned;     /* types already warned about */
    const char *what;
} VmstateCheck;

static int check_device_vmstate(Object *obj, void *opaque)
{
    VmstateCheck *c = opaque;
    DeviceState *dev = (DeviceState *)object_dynamic_cast(obj, TYPE_DEVICE);
    const char *type, *origin;

    if (!dev || !dev->realized) {
        return 0;
    }
    type = object_get_typename(obj);
    origin = panda_module_type_origin(type);
    if (origin && !DEVICE_GET_CLASS(dev)->vmsd &&
        g_hash_table_add(c->warned, (gpointer)type)) {
        g_autofree char *path = object_get_canonical_path(obj);

        warn_report("device %s (%s, from %s) has no vmstate: %s will not "
                    "save or restore it", path, type, origin, c->what);
    }
    return 0;
}

void panda_modules_check_vmstate(const char *what)
{
    static GHashTable *warned;
    VmstateCheck c;

    if (!module_types || !g_hash_table_size(module_types)) {
        return;
    }
    if (!warned) {
        warned = g_hash_table_new(g_str_hash, g_str_equal);
    }
    c.warned = warned;
    c.what = what;
    object_child_foreach_recursive(object_get_root(), check_device_vmstate,
                                   &c);
}
