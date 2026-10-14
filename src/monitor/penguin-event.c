/*
 * Penguin raw QMP event emission.
 *
 * Lets a Penguin plugin push an unsolicited QMP event of any name to every
 * connected monitor -- the outbound counterpart to the QMP command callback in
 * system/penguin.c.
 *
 * This lives under monitor/ rather than next to the other Penguin hooks
 * because it needs monitor-internal.h: emitting an event by name means walking
 * mon_list directly instead of going through qapi_event_emit(), which is keyed
 * by QAPIEvent ordinal.
 */

#include "qemu/osdep.h"
#include "monitor-internal.h"
#include "qapi/error.h"
#include "qapi/qmp-event.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "system/penguin.h"

/*
 * Broadcast an already-built event dict to every QMP monitor in command mode.
 *
 * Mirrors monitor_qapi_event_emit() and the QMP class's emit_event hook
 * (monitor_qmp_emit_event() in monitor/qmp.c), but deliberately bypasses
 * qapi_event_queue()/monitor_qapi_event_conf[]: those are keyed by QAPIEvent
 * ordinals, and a Penguin plugin emits events by name, with no schema entry and
 * so no ordinal. Rate limiting is therefore not applied -- Penguin events are
 * never throttled or coalesced.
 *
 * @qdict must already be a complete event object. The caller retains its
 * reference.
 */
static void penguin_monitor_broadcast_event(QDict *qdict)
{
    Monitor *mon;
    MonitorQMP *qmp_mon;

    QEMU_LOCK_GUARD(&monitor_lock);

    QTAILQ_FOREACH(mon, &mon_list, entry) {
        qmp_mon = MONITOR_QMP(
            object_dynamic_cast(OBJECT(mon), TYPE_MONITOR_QMP));
        if (!qmp_mon) {
            continue;
        }

        {
            QEMU_LOCK_GUARD(&mon->mon_lock);
            /*
             * Still negotiating capabilities: the client has not sent
             * qmp_capabilities yet and must not receive events.
             */
            if (qmp_mon->commands == &qmp_cap_negotiation_commands) {
                continue;
            }
        }
        qmp_send_response(qmp_mon, qdict);
    }
}

/*
 * A deferred emission: the event name plus its optional JSON payload. Two
 * fields, so a struct rather than a bare pointer for the bottom half's opaque.
 */
typedef struct PenguinEventReq {
    char *name;
    char *data_json;
} PenguinEventReq;

/*
 * Main-loop half of penguin_qmp_emit_event(). Parses and validates the payload,
 * then broadcasts the event.
 *
 * Ownership of @opaque transfers here, so this frees it on every path.
 */
static void penguin_qmp_emit_event_bh(void *opaque)
{
    PenguinEventReq *req = opaque;
    QObject *data = NULL;
    QDict *event = NULL;
    Error *err = NULL;

    if (req->data_json && req->data_json[0]) {
        data = qobject_from_json(req->data_json, &err);
        if (err) {
            error_reportf_err(err, "penguin: dropping QMP event \"%s\": "
                                   "invalid JSON payload: ", req->name);
            goto out;
        }
        /*
         * The QMP spec defines an event's "data" member as a json-object (see
         * docs/interop/qmp-spec.rst), so reject a scalar or array rather than
         * emitting something no conforming client can parse.
         */
        if (qobject_type(data) != QTYPE_QDICT) {
            error_report("penguin: dropping QMP event \"%s\": payload must be "
                         "a JSON object, not a scalar or array", req->name);
            goto out;
        }
    }

    /*
     * Build through the normal helper, so the "event" member and the timestamp
     * are formatted exactly as they are for a schema-defined event.
     */
    event = qmp_event_build_dict(req->name);
    if (data) {
        /* Ownership of @data transfers into @event. */
        qdict_put_obj(event, "data", data);
        data = NULL;
    }

    penguin_monitor_broadcast_event(event);

out:
    qobject_unref(event);
    qobject_unref(data);
    g_free(req->name);
    g_free(req->data_json);
    g_free(req);
}

void __attribute__((visibility("default")))
penguin_qmp_emit_event(const char *name, const char *data_json)
{
    PenguinEventReq *req;

    if (!name || !name[0]) {
        error_report("penguin: refusing to emit QMP event with empty name");
        return;
    }

    req = g_new0(PenguinEventReq, 1);
    req->name = g_strdup(name);
    req->data_json = g_strdup(data_json);

    /*
     * Defer to the main loop: the broadcast takes monitor_lock and writes to
     * each monitor chardev, and deferring keeps the calling vCPU thread from
     * blocking on JSON parsing + write.
     */
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            penguin_qmp_emit_event_bh, req);
}
