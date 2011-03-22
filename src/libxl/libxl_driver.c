/*---------------------------------------------------------------------------*/
/*  Copyright (c) 2011 SUSE LINUX Products GmbH, Nuernberg, Germany.
 *  Copyright (C) 2011 Univention GmbH.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */
/*---------------------------------------------------------------------------*/

#include <config.h>

#include <sys/utsname.h>
#include <libxl.h>

#include "internal.h"
#include "logging.h"
#include "virterror_internal.h"
#include "datatypes.h"
#include "files.h"
#include "memory.h"
#include "event.h"
#include "uuid.h"
#include "command.h"
#include "libxl_driver.h"
#include "libxl_conf.h"


#define VIR_FROM_THIS VIR_FROM_LIBXL

#define LIBXL_DOM_REQ_POWEROFF 0
#define LIBXL_DOM_REQ_REBOOT   1
#define LIBXL_DOM_REQ_SUSPEND  2
#define LIBXL_DOM_REQ_CRASH    3
#define LIBXL_DOM_REQ_HALT     4

static libxlDriverPrivatePtr libxl_driver = NULL;


/* Function declarations */
static int
libxlVmStart(libxlDriverPrivatePtr driver,
             virDomainObjPtr vm, bool start_paused);


/* Function definitions */
static void
libxlDriverLock(libxlDriverPrivatePtr driver)
{
    virMutexLock(&driver->lock);
}

static void
libxlDriverUnlock(libxlDriverPrivatePtr driver)
{
    virMutexUnlock(&driver->lock);
}

static void *
libxlDomainObjPrivateAlloc(void)
{
    libxlDomainObjPrivatePtr priv;

    if (VIR_ALLOC(priv) < 0)
        return NULL;

    libxl_ctx_init(&priv->ctx, LIBXL_VERSION, libxl_driver->logger);
    priv->waiterFD = -1;
    priv->eventHdl = -1;

    return priv;
}

static void
libxlDomainObjPrivateFree(void *data)
{
    libxlDomainObjPrivatePtr priv = data;

    if (priv->eventHdl >= 0)
        virEventRemoveHandle(priv->eventHdl);

    if (priv->dWaiter) {
        libxl_stop_waiting(&priv->ctx, priv->dWaiter);
        libxl_free_waiter(priv->dWaiter);
        VIR_FREE(priv->dWaiter);
    }

    libxl_ctx_free(&priv->ctx);
    VIR_FREE(priv);
}

/*
 * Remove reference to domain object.
 */
static void
libxlDomainObjUnref(void *data)
{
    virDomainObjPtr vm = data;

    virDomainObjUnref(vm);
}

/*
 * Cleanup function for domain that has reached shutoff state.
 *
 * virDomainObjPtr should be locked on invocation
 */
static void
libxlVmCleanup(libxlDriverPrivatePtr driver, virDomainObjPtr vm)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;
    int vnc_port;
    char *file;

    if (priv->eventHdl >= 0) {
        virEventRemoveHandle(priv->eventHdl);
        priv->eventHdl = -1;
    }

    if (priv->dWaiter) {
        libxl_stop_waiting(&priv->ctx, priv->dWaiter);
        libxl_free_waiter(priv->dWaiter);
        VIR_FREE(priv->dWaiter);
    }

    if (vm->persistent) {
        vm->def->id = -1;
        vm->state = VIR_DOMAIN_SHUTOFF;
    }

    if ((vm->def->ngraphics == 1) &&
        vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
        vm->def->graphics[0]->data.vnc.autoport) {
        vnc_port = vm->def->graphics[0]->data.vnc.port;
        if (vnc_port >= LIBXL_VNC_PORT_MIN) {
            if (virBitmapClearBit(driver->reservedVNCPorts,
                                  vnc_port - LIBXL_VNC_PORT_MIN) < 0)
                VIR_DEBUG("Could not mark port %d as unused", vnc_port);
        }
    }

    if (virAsprintf(&file, "%s/%s.xml", driver->stateDir, vm->def->name) > 0) {
        if (unlink(file) < 0 && errno != ENOENT && errno != ENOTDIR)
            VIR_DEBUG("Failed to remove domain XML for %s", vm->def->name);
        VIR_FREE(file);
    }
}

/*
 * Reap a domain from libxenlight.
 *
 * virDomainObjPtr should be locked on invocation
 */
static int
libxlVmReap(libxlDriverPrivatePtr driver, virDomainObjPtr vm, int force)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;

    if (libxl_domain_destroy(&priv->ctx, vm->def->id, force) < 0) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("Unable to cleanup domain %d"), vm->def->id);
        return -1;
    }

    libxlVmCleanup(driver, vm);
    return 0;
}

/*
 * Handle previously registered event notification from libxenlight
 */
static void libxlEventHandler(int watch,
                              int fd,
                              int events,
                              void *data)
{
    libxlDriverPrivatePtr driver = libxl_driver;
    virDomainObjPtr vm = data;
    libxlDomainObjPrivatePtr priv;
    libxl_event event;
    libxl_dominfo info;

    libxlDriverLock(driver);
    virDomainObjLock(vm);
    libxlDriverUnlock(driver);

    priv = vm->privateData;

    memset(&event, 0, sizeof(event));
    memset(&info, 0, sizeof(info));

    if (priv->waiterFD != fd || priv->eventHdl != watch) {
        virEventRemoveHandle(watch);
        priv->eventHdl = -1;
        goto cleanup;
    }

    if (!(events & VIR_EVENT_HANDLE_READABLE))
        goto cleanup;

    if (libxl_get_event(&priv->ctx, &event))
        goto cleanup;

    if (event.type == LIBXL_EVENT_DOMAIN_DEATH) {
        /* libxl_event_get_domain_death_info returns 1 if death
         * event was for this domid */
        if (libxl_event_get_domain_death_info(&priv->ctx,
                                              vm->def->id,
                                              &event,
                                              &info) != 1)
            goto cleanup;

        virEventRemoveHandle(watch);
        priv->eventHdl = -1;
        switch (info.shutdown_reason) {
            case SHUTDOWN_poweroff:
            case SHUTDOWN_crash:
                libxlVmReap(driver, vm, 0);
                if (!vm->persistent) {
                    virDomainRemoveInactive(&driver->domains, vm);
                    vm = NULL;
                }
                break;
            case SHUTDOWN_reboot:
                libxlVmReap(driver, vm, 0);
                libxlVmStart(driver, vm, 0);
                break;
            default:
                VIR_INFO("Unhandled shutdown_reason %d", info.shutdown_reason);
                break;
        }
    }

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    libxl_free_event(&event);
}

/*
 * Register domain events with libxenlight and insert event handles
 * in libvirt's event loop.
 */
static int
libxlCreateDomEvents(virDomainObjPtr vm)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;
    int fd;

    if (VIR_ALLOC(priv->dWaiter) < 0) {
        virReportOOMError();
        return -1;
    }

    if (libxl_wait_for_domain_death(&priv->ctx, vm->def->id, priv->dWaiter))
        goto error;

    libxl_get_wait_fd(&priv->ctx, &fd);
    if (fd < 0)
        goto error;

    priv->waiterFD = fd;
    /* Add a reference to the domain object while it is injected in
     * the event loop.
     */
    virDomainObjRef(vm);
    if ((priv->eventHdl = virEventAddHandle(
             fd,
             VIR_EVENT_HANDLE_READABLE | VIR_EVENT_HANDLE_ERROR,
             libxlEventHandler,
             vm, libxlDomainObjUnref)) < 0) {
        virDomainObjUnref(vm);
        goto error;
    }

    return 0;

error:
    libxl_free_waiter(priv->dWaiter);
    VIR_FREE(priv->dWaiter);
    priv->eventHdl = -1;
    return -1;
}

/*
 * Start a domain through libxenlight.
 *
 * virDomainObjPtr should be locked on invocation
 */
static int
libxlVmStart(libxlDriverPrivatePtr driver,
             virDomainObjPtr vm, bool start_paused)
{
    libxl_domain_config d_config;
    virDomainDefPtr def = vm->def;
    int ret;
    uint32_t domid = 0;
    char *dom_xml = NULL;
    pid_t child_console_pid = -1;
    libxlDomainObjPrivatePtr priv = vm->privateData;

    memset(&d_config, 0, sizeof(d_config));

    if (libxlBuildDomainConfig(driver, def, &d_config) < 0 )
        return -1;

    //TODO: Balloon dom0 ??
    //ret = freemem(&d_config->b_info, &d_config->dm_info);

    ret = libxl_domain_create_new(&priv->ctx, &d_config,
                                  NULL, &child_console_pid, &domid);
    if (ret) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("libxenlight failed to create new domain '%s'"),
                   d_config.c_info.name);
        goto error;
    }

    def->id = domid;
    if ((dom_xml = virDomainDefFormat(def, 0)) == NULL)
        goto error;

    if (libxl_userdata_store(&priv->ctx, domid, "libvirt-xml",
                             (uint8_t *)dom_xml, strlen(dom_xml) + 1)) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("libxenlight failed to store userdata"));
        goto error;
    }

    if (libxlCreateDomEvents(vm) < 0)
        goto error;

    if (!start_paused) {
        libxl_domain_unpause(&priv->ctx, domid);
        vm->state = VIR_DOMAIN_RUNNING;
    } else {
        vm->state = VIR_DOMAIN_PAUSED;
    }

    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
        goto error;

    libxl_domain_config_destroy(&d_config);
    VIR_FREE(dom_xml);
    return 0;

error:
    if (domid > 0) {
        libxl_domain_destroy(&priv->ctx, domid, 0);
        def->id = -1;
        vm->state = VIR_DOMAIN_SHUTOFF;
    }
    libxl_domain_config_destroy(&d_config);
    VIR_FREE(dom_xml);
    return -1;
}


/*
 * Reconnect to running domains that were previously started/created
 * with libxenlight driver.
 */
static void
libxlReconnectDomain(void *payload,
                     const void *name ATTRIBUTE_UNUSED,
                     void *opaque)
{
    virDomainObjPtr vm = payload;
    libxlDriverPrivatePtr driver = opaque;
    int rc;
    libxl_dominfo d_info;
    int len;
    uint8_t *data = NULL;

    virDomainObjLock(vm);

    /* Does domain still exist? */
    rc = libxl_domain_info(&driver->ctx, &d_info, vm->def->id);
    if (rc == ERROR_INVAL) {
        goto out;
    } else if (rc != 0) {
        VIR_DEBUG("libxl_domain_info failed (code %d), ignoring domain %d",
                  rc, vm->def->id);
        goto out;
    }

    /* Is this a domain that was under libvirt control? */
    if (libxl_userdata_retrieve(&driver->ctx, vm->def->id,
                                "libvirt-xml", &data, &len)) {
        VIR_DEBUG("libxl_userdata_retrieve failed, ignoring domain %d", vm->def->id);
        goto out;
    }

    /* Update domid in case it changed (e.g. reboot) while we were gone? */
    vm->def->id = d_info.domid;
    vm->state = VIR_DOMAIN_RUNNING;

    /* Recreate domain death et. al. events */
    libxlCreateDomEvents(vm);
    virDomainObjUnlock(vm);
    return;

out:
    libxlVmCleanup(driver, vm);
    if (!vm->persistent)
        virDomainRemoveInactive(&driver->domains, vm);
    else
        virDomainObjUnlock(vm);
}

static void
libxlReconnectDomains(libxlDriverPrivatePtr driver)
{
    virHashForEach(driver->domains.objs, libxlReconnectDomain, driver);
}

static int
libxlShutdown(void)
{
    if (!libxl_driver)
        return -1;

    libxlDriverLock(libxl_driver);
    virCapabilitiesFree(libxl_driver->caps);
    virDomainObjListDeinit(&libxl_driver->domains);
    libxl_ctx_free(&libxl_driver->ctx);
    xtl_logger_destroy(libxl_driver->logger);
    if (libxl_driver->logger_file)
        VIR_FORCE_FCLOSE(libxl_driver->logger_file);

    virBitmapFree(libxl_driver->reservedVNCPorts);

    VIR_FREE(libxl_driver->configDir);
    VIR_FREE(libxl_driver->autostartDir);
    VIR_FREE(libxl_driver->logDir);
    VIR_FREE(libxl_driver->stateDir);
    VIR_FREE(libxl_driver->libDir);
    VIR_FREE(libxl_driver->saveDir);

    libxlDriverUnlock(libxl_driver);
    virMutexDestroy(&libxl_driver->lock);
    VIR_FREE(libxl_driver);

    return 0;
}

static int
libxlStartup(int privileged) {
    const libxl_version_info *ver_info;
    char *log_file = NULL;
    virCommandPtr cmd;
    int status;

    /* Disable libxl driver if non-root */
    if (!privileged) {
        VIR_INFO0("Not running privileged, disabling libxenlight driver");
        return 0;
    }

    /* Disable driver if legacy xen toolstack (xend) is in use */
    cmd = virCommandNewArgList("/usr/sbin/xend", "status", NULL);
    if (virCommandRun(cmd, &status) == 0 && status == 0) {
        VIR_INFO0("Legacy xen tool stack seems to be in use, disabling "
                  "libxenlight driver.");
        virCommandFree(cmd);
        return 0;
    }
    virCommandFree(cmd);

    if (VIR_ALLOC(libxl_driver) < 0)
        return -1;

    if (virMutexInit(&libxl_driver->lock) < 0) {
        VIR_ERROR0(_("cannot initialize mutex"));
        VIR_FREE(libxl_driver);
        return -1;
    }
    libxlDriverLock(libxl_driver);

    /* Allocate bitmap for vnc port reservation */
    if ((libxl_driver->reservedVNCPorts =
         virBitmapAlloc(LIBXL_VNC_PORT_MAX - LIBXL_VNC_PORT_MIN)) == NULL)
        goto out_of_memory;

    if (virDomainObjListInit(&libxl_driver->domains) < 0)
        goto out_of_memory;

    if (virAsprintf(&libxl_driver->configDir,
                    "%s", LIBXL_CONFIG_DIR) == -1)
        goto out_of_memory;

    if (virAsprintf(&libxl_driver->autostartDir,
                    "%s", LIBXL_AUTOSTART_DIR) == -1)
        goto out_of_memory;

    if (virAsprintf(&libxl_driver->logDir,
                    "%s", LIBXL_LOG_DIR) == -1)
        goto out_of_memory;

    if (virAsprintf(&libxl_driver->stateDir,
                    "%s", LIBXL_STATE_DIR) == -1)
        goto out_of_memory;

    if (virAsprintf(&libxl_driver->libDir,
                    "%s", LIBXL_LIB_DIR) == -1)
        goto out_of_memory;

    if (virAsprintf(&libxl_driver->saveDir,
                    "%s", LIBXL_SAVE_DIR) == -1)
        goto out_of_memory;

    if (virFileMakePath(libxl_driver->logDir) != 0) {
        char ebuf[1024];
        VIR_ERROR(_("Failed to create log dir '%s': %s"),
                  libxl_driver->logDir, virStrerror(errno, ebuf, sizeof ebuf));
        goto error;
    }
    if (virFileMakePath(libxl_driver->stateDir) != 0) {
        char ebuf[1024];
        VIR_ERROR(_("Failed to create state dir '%s': %s"),
                  libxl_driver->stateDir, virStrerror(errno, ebuf, sizeof ebuf));
        goto error;
    }
    if (virFileMakePath(libxl_driver->libDir) != 0) {
        char ebuf[1024];
        VIR_ERROR(_("Failed to create lib dir '%s': %s"),
                  libxl_driver->libDir, virStrerror(errno, ebuf, sizeof ebuf));
        goto error;
    }
    if (virFileMakePath(libxl_driver->saveDir) != 0) {
        char ebuf[1024];
        VIR_ERROR(_("Failed to create save dir '%s': %s"),
                  libxl_driver->saveDir, virStrerror(errno, ebuf, sizeof ebuf));
        goto error;
    }

    if (virAsprintf(&log_file, "%s/libxl.log", libxl_driver->logDir) < 0) {
        goto out_of_memory;
    }

    if ((libxl_driver->logger_file = fopen(log_file, "a")) == NULL)  {
        virReportSystemError(errno,
                             _("failed to create logfile %s"),
                             log_file);
        goto error;
    }
    VIR_FREE(log_file);

    libxl_driver->logger =
            (xentoollog_logger *)xtl_createlogger_stdiostream(libxl_driver->logger_file, XTL_DEBUG,  0);
    if (!libxl_driver->logger) {
        VIR_ERROR0(_("cannot create logger for libxenlight"));
        goto error;
    }

    if (libxl_ctx_init(&libxl_driver->ctx,
                       LIBXL_VERSION,
                       libxl_driver->logger)) {
        VIR_ERROR0(_("cannot initialize libxenlight context"));
        goto error;
    }

    if ((ver_info = libxl_get_version_info(&libxl_driver->ctx)) == NULL) {
        VIR_ERROR0(_("cannot version information from libxenlight"));
        goto error;
    }
    libxl_driver->version = (ver_info->xen_version_major * 1000000) +
            (ver_info->xen_version_minor * 1000);

    if ((libxl_driver->caps =
         libxlMakeCapabilities(&libxl_driver->ctx)) == NULL) {
        VIR_ERROR0(_("cannot create capabilities for libxenlight"));
        goto error;
    }

    libxl_driver->caps->privateDataAllocFunc = libxlDomainObjPrivateAlloc;
    libxl_driver->caps->privateDataFreeFunc = libxlDomainObjPrivateFree;

    /* Load running domains first. */
    if (virDomainLoadAllConfigs(libxl_driver->caps,
                                &libxl_driver->domains,
                                libxl_driver->stateDir,
                                libxl_driver->autostartDir,
                                1, NULL, NULL) < 0)
        goto error;

    libxlReconnectDomains(libxl_driver);

    /* Then inactive persistent configs */
    if (virDomainLoadAllConfigs(libxl_driver->caps,
                                &libxl_driver->domains,
                                libxl_driver->configDir,
                                libxl_driver->autostartDir,
                                0, NULL, NULL) < 0)
        goto error;

    libxlDriverUnlock(libxl_driver);

    /* TODO: autostart domains */

    return 0;

out_of_memory:
    virReportOOMError();
error:
    VIR_FREE(log_file);
    if (libxl_driver)
        libxlDriverUnlock(libxl_driver);
    libxlShutdown();
    return -1;
}

static int
libxlReload(void)
{
    if (!libxl_driver)
        return 0;

    libxlDriverLock(libxl_driver);
    virDomainLoadAllConfigs(libxl_driver->caps,
                            &libxl_driver->domains,
                            libxl_driver->configDir,
                            libxl_driver->autostartDir,
                            0, NULL, libxl_driver);
    libxlDriverUnlock(libxl_driver);

    /* TODO: autostart domains */

    return 0;
}

static int
libxlActive(void)
{
    if (!libxl_driver)
        return 0;

    return 1;
}

static virDrvOpenStatus
libxlOpen(virConnectPtr conn,
          virConnectAuthPtr auth ATTRIBUTE_UNUSED,
          int flags ATTRIBUTE_UNUSED)
{
    if (conn->uri == NULL) {
        if (libxl_driver == NULL)
            return VIR_DRV_OPEN_DECLINED;

        conn->uri = xmlParseURI("xen:///");
        if (!conn->uri) {
            virReportOOMError();
            return VIR_DRV_OPEN_ERROR;
        }
    } else {
        /* Only xen scheme */
        if (conn->uri->scheme == NULL || STRNEQ(conn->uri->scheme, "xen"))
            return VIR_DRV_OPEN_DECLINED;

        /* If server name is given, its for remote driver */
        if (conn->uri->server != NULL)
            return VIR_DRV_OPEN_DECLINED;

        /* Error if xen or libxl scheme specified but driver not started. */
        if (libxl_driver == NULL) {
            libxlError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("libxenlight state driver is not active"));
            return VIR_DRV_OPEN_ERROR;
        }

        /* /session isn't supported in libxenlight */
        if (conn->uri->path &&
            STRNEQ(conn->uri->path, "") &&
            STRNEQ(conn->uri->path, "/") &&
            STRNEQ(conn->uri->path, "/system")) {
            libxlError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected Xen URI path '%s', try xen:///"),
                       NULLSTR(conn->uri->path));
            return VIR_DRV_OPEN_ERROR;
        }
    }

    conn->privateData = libxl_driver;

    return VIR_DRV_OPEN_SUCCESS;
};

static int
libxlClose(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    conn->privateData = NULL;
    return 0;
}

static const char *
libxlGetType(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return "xenlight";
}

static int
libxlGetVersion(virConnectPtr conn, unsigned long *version)
{
    libxlDriverPrivatePtr driver = conn->privateData;

    libxlDriverLock(driver);
    *version = driver->version;
    libxlDriverUnlock(driver);
    return 0;
}

static int
libxlGetMaxVcpus(virConnectPtr conn, const char *type ATTRIBUTE_UNUSED)
{
    int ret;
    libxlDriverPrivatePtr driver = conn->privateData;

    ret = libxl_get_max_cpus(&driver->ctx);
    /* libxl_get_max_cpus() will return 0 if there were any failures,
       e.g. xc_physinfo() failing */
    if (ret == 0)
        return -1;

    return ret;
}

static int
libxlNodeGetInfo(virConnectPtr conn, virNodeInfoPtr info)
{
    libxl_physinfo phy_info;
    const libxl_version_info* ver_info;
    libxlDriverPrivatePtr driver = conn->privateData;
    struct utsname utsname;

    if (libxl_get_physinfo(&driver->ctx, &phy_info)) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("libxl_get_physinfo_info failed"));
        return -1;
    }

    if ((ver_info = libxl_get_version_info(&driver->ctx)) == NULL) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("libxl_get_version_info failed"));
        return -1;
    }

    uname(&utsname);
    if (virStrncpy(info->model,
                   utsname.machine,
                   strlen(utsname.machine),
                   sizeof(info->model)) == NULL) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("machine type %s too big for destination"),
                   utsname.machine);
        return -1;
    }

    info->memory = phy_info.total_pages * (ver_info->pagesize / 1024);
    info->cpus = phy_info.nr_cpus;
    info->nodes = phy_info.nr_nodes;
    info->cores = phy_info.cores_per_socket;
    info->threads = phy_info.threads_per_core;
    info->sockets = 1;
    info->mhz = phy_info.cpu_khz / 1000;
    return 0;
}

static char *
libxlGetCapabilities(virConnectPtr conn)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    char *xml;

    libxlDriverLock(driver);
    if ((xml = virCapabilitiesFormatXML(driver->caps)) == NULL)
        virReportOOMError();
    libxlDriverUnlock(driver);

    return xml;
}

static int
libxlListDomains(virConnectPtr conn, int *ids, int nids)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    int n;

    libxlDriverLock(driver);
    n = virDomainObjListGetActiveIDs(&driver->domains, ids, nids);
    libxlDriverUnlock(driver);

    return n;
}

static int
libxlNumDomains(virConnectPtr conn)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    int n;

    libxlDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 1);
    libxlDriverUnlock(driver);

    return n;
}

static virDomainPtr
libxlDomainCreateXML(virConnectPtr conn, const char *xml,
                     unsigned int flags)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    virDomainDefPtr def;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;

    virCheckFlags(VIR_DOMAIN_START_PAUSED, NULL);

    libxlDriverLock(driver);
    if (!(def = virDomainDefParseString(driver->caps, xml,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

    if (virDomainObjIsDuplicate(&driver->domains, def, 1) < 0)
        goto cleanup;

    if (!(vm = virDomainAssignDef(driver->caps,
                                  &driver->domains, def, false)))
        goto cleanup;
    def = NULL;

    if (libxlVmStart(driver, vm, (flags & VIR_DOMAIN_START_PAUSED) != 0) < 0) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    libxlDriverUnlock(driver);
    return dom;
}

static virDomainPtr
libxlDomainLookupByID(virConnectPtr conn, int id)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    libxlDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, id);
    libxlDriverUnlock(driver);

    if (!vm) {
        libxlError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

  cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr
libxlDomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, uuid);
    libxlDriverUnlock(driver);

    if (!vm) {
        libxlError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

  cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr
libxlDomainLookupByName(virConnectPtr conn, const char *name)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    libxlDriverLock(driver);
    vm = virDomainFindByName(&driver->domains, name);
    libxlDriverUnlock(driver);

    if (!vm) {
        libxlError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

  cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static int
libxlDomainShutdown(virDomainPtr dom)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    libxlDomainObjPrivatePtr priv;

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        libxlError(VIR_ERR_NO_DOMAIN,
                   _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        libxlError(VIR_ERR_OPERATION_INVALID,
                   "%s", _("Domain is not running"));
        goto cleanup;
    }

    priv = vm->privateData;
    if (libxl_domain_shutdown(&priv->ctx, dom->id, LIBXL_DOM_REQ_POWEROFF) != 0) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("Failed to shutdown domain '%d' with libxenlight"),
                   dom->id);
        goto cleanup;
    }

    /* vm is marked shutoff (or removed from domains list if not persistent)
     * in shutdown event handler.
     */
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    libxlDriverUnlock(driver);
    return ret;
}

static int
libxlDomainReboot(virDomainPtr dom, unsigned int flags ATTRIBUTE_UNUSED)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    libxlDomainObjPrivatePtr priv;

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        libxlError(VIR_ERR_NO_DOMAIN,
                   _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        libxlError(VIR_ERR_OPERATION_INVALID,
                   "%s", _("Domain is not running"));
        goto cleanup;
    }

    priv = vm->privateData;
    if (libxl_domain_shutdown(&priv->ctx, dom->id, LIBXL_DOM_REQ_REBOOT) != 0) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("Failed to reboot domain '%d' with libxenlight"),
                   dom->id);
        goto cleanup;
    }
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    libxlDriverUnlock(driver);
    return ret;
}

static int
libxlDomainDestroy(virDomainPtr dom)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    libxlDomainObjPrivatePtr priv;

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        libxlError(VIR_ERR_NO_DOMAIN,
                   _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        libxlError(VIR_ERR_OPERATION_INVALID,
                   "%s", _("Domain is not running"));
        goto cleanup;
    }

    priv = vm->privateData;
    if (libxlVmReap(driver, vm, 1) != 0) {
        libxlError(VIR_ERR_INTERNAL_ERROR,
                   _("Failed to destroy domain '%d'"), dom->id);
        goto cleanup;
    }

    if (!vm->persistent) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
    }

    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    libxlDriverUnlock(driver);
    return ret;
}

static int
libxlDomainGetInfo(virDomainPtr dom, virDomainInfoPtr info)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    libxlDriverUnlock(driver);

    if (!vm) {
        libxlError(VIR_ERR_NO_DOMAIN, "%s",
                   _("no domain with matching uuid"));
        goto cleanup;
    }

    info->state = vm->state;
    info->cpuTime = 0;
    info->maxMem = vm->def->mem.max_balloon;
    info->memory = vm->def->mem.cur_balloon;
    info->nrVirtCpu = vm->def->vcpus;
    ret = 0;

  cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static char *
libxlDomainDumpXML(virDomainPtr dom, int flags)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *ret = NULL;

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    libxlDriverUnlock(driver);

    if (!vm) {
        libxlError(VIR_ERR_NO_DOMAIN, "%s",
                   _("no domain with matching uuid"));
        goto cleanup;
    }

    ret = virDomainDefFormat(vm->def, flags);

  cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int
libxlListDefinedDomains(virConnectPtr conn,
                        char **const names, int nnames)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    int n;

    libxlDriverLock(driver);
    n = virDomainObjListGetInactiveNames(&driver->domains, names, nnames);
    libxlDriverUnlock(driver);
    return n;
}

static int
libxlNumDefinedDomains(virConnectPtr conn)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    int n;

    libxlDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 0);
    libxlDriverUnlock(driver);

    return n;
}

static int
libxlDomainCreateWithFlags(virDomainPtr dom,
                           unsigned int flags ATTRIBUTE_UNUSED)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_START_PAUSED, -1);

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        libxlError(VIR_ERR_NO_DOMAIN,
                   _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        libxlError(VIR_ERR_OPERATION_INVALID,
                   "%s", _("Domain is already running"));
        goto cleanup;
    }

    ret = libxlVmStart(driver, vm, (flags & VIR_DOMAIN_START_PAUSED) != 0);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    libxlDriverUnlock(driver);
    return ret;
}

static int
libxlDomainCreate(virDomainPtr dom)
{
    return libxlDomainCreateWithFlags(dom, 0);
}

static virDomainPtr
libxlDomainDefineXML(virConnectPtr conn, const char *xml)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;
    int dupVM;

    libxlDriverLock(driver);
    if (!(def = virDomainDefParseString(driver->caps, xml,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

   if ((dupVM = virDomainObjIsDuplicate(&driver->domains, def, 0)) < 0)
        goto cleanup;

    if (!(vm = virDomainAssignDef(driver->caps,
                                  &driver->domains, def, false)))
        goto cleanup;
    def = NULL;
    vm->persistent = 1;

    if (virDomainSaveConfig(driver->configDir,
                            vm->newDef ? vm->newDef : vm->def) < 0) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    libxlDriverUnlock(driver);
    return dom;
}

static int
libxlDomainUndefine(virDomainPtr dom)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    libxlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];

        virUUIDFormat(dom->uuid, uuidstr);
        libxlError(VIR_ERR_NO_DOMAIN,
                   _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        libxlError(VIR_ERR_OPERATION_INVALID,
                   "%s", _("cannot undefine active domain"));
        goto cleanup;
    }

    if (!vm->persistent) {
        libxlError(VIR_ERR_OPERATION_INVALID,
                   "%s", _("cannot undefine transient domain"));
        goto cleanup;
    }

    if (virDomainDeleteConfig(driver->configDir,
                              driver->autostartDir,
                              vm) < 0)
        goto cleanup;

    virDomainRemoveInactive(&driver->domains, vm);
    vm = NULL;
    ret = 0;

  cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    libxlDriverUnlock(driver);
    return ret;
}

static int
libxlDomainIsActive(virDomainPtr dom)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    libxlDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    libxlDriverUnlock(driver);
    if (!obj) {
        libxlError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }
    ret = virDomainObjIsActive(obj);

  cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}

static int
libxlDomainIsPersistent(virDomainPtr dom)
{
    libxlDriverPrivatePtr driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    libxlDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    libxlDriverUnlock(driver);
    if (!obj) {
        libxlError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }
    ret = obj->persistent;

  cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}


static virDriver libxlDriver = {
    VIR_DRV_LIBXL,
    "xenlight",
    libxlOpen,                  /* open */
    libxlClose,                 /* close */
    NULL,                       /* supports_feature */
    libxlGetType,               /* type */
    libxlGetVersion,            /* version */
    NULL,                       /* libvirtVersion (impl. in libvirt.c) */
    virGetHostname,             /* getHostname */
    NULL,                       /* getSysinfo */
    libxlGetMaxVcpus,           /* getMaxVcpus */
    libxlNodeGetInfo,           /* nodeGetInfo */
    libxlGetCapabilities,       /* getCapabilities */
    libxlListDomains,           /* listDomains */
    libxlNumDomains,            /* numOfDomains */
    libxlDomainCreateXML,       /* domainCreateXML */
    libxlDomainLookupByID,      /* domainLookupByID */
    libxlDomainLookupByUUID,    /* domainLookupByUUID */
    libxlDomainLookupByName,    /* domainLookupByName */
    NULL,                       /* domainSuspend */
    NULL,                       /* domainResume */
    libxlDomainShutdown,        /* domainShutdown */
    libxlDomainReboot,          /* domainReboot */
    libxlDomainDestroy,         /* domainDestroy */
    NULL,                       /* domainGetOSType */
    NULL,                       /* domainGetMaxMemory */
    NULL,                       /* domainSetMaxMemory */
    NULL,                       /* domainSetMemory */
    NULL,                       /* domainSetMemoryFlags */
    NULL,                       /* domainSetMemoryParameters */
    NULL,                       /* domainGetMemoryParameters */
    NULL,                       /* domainSetBlkioParameters */
    NULL,                       /* domainGetBlkioParameters */
    libxlDomainGetInfo,         /* domainGetInfo */
    NULL,                       /* domainSave */
    NULL,                       /* domainRestore */
    NULL,                       /* domainCoreDump */
    NULL,                       /* domainSetVcpus */
    NULL,                       /* domainSetVcpusFlags */
    NULL,                       /* domainGetVcpusFlags */
    NULL,                       /* domainPinVcpu */
    NULL,                       /* domainGetVcpus */
    NULL,                       /* domainGetMaxVcpus */
    NULL,                       /* domainGetSecurityLabel */
    NULL,                       /* nodeGetSecurityModel */
    libxlDomainDumpXML,         /* domainDumpXML */
    NULL,                       /* domainXmlFromNative */
    NULL,                       /* domainXmlToNative */
    libxlListDefinedDomains,    /* listDefinedDomains */
    libxlNumDefinedDomains,     /* numOfDefinedDomains */
    libxlDomainCreate,          /* domainCreate */
    libxlDomainCreateWithFlags, /* domainCreateWithFlags */
    libxlDomainDefineXML,       /* domainDefineXML */
    libxlDomainUndefine,        /* domainUndefine */
    NULL,                       /* domainAttachDevice */
    NULL,                       /* domainAttachDeviceFlags */
    NULL,                       /* domainDetachDevice */
    NULL,                       /* domainDetachDeviceFlags */
    NULL,                       /* domainUpdateDeviceFlags */
    NULL,                       /* domainGetAutostart */
    NULL,                       /* domainSetAutostart */
    NULL,                       /* domainGetSchedulerType */
    NULL,                       /* domainGetSchedulerParameters */
    NULL,                       /* domainSetSchedulerParameters */
    NULL,                       /* domainMigratePrepare */
    NULL,                       /* domainMigratePerform */
    NULL,                       /* domainMigrateFinish */
    NULL,                       /* domainBlockStats */
    NULL,                       /* domainInterfaceStats */
    NULL,                       /* domainMemoryStats */
    NULL,                       /* domainBlockPeek */
    NULL,                       /* domainMemoryPeek */
    NULL,                       /* domainGetBlockInfo */
    NULL,                       /* nodeGetCellsFreeMemory */
    NULL,                       /* getFreeMemory */
    NULL,                       /* domainEventRegister */
    NULL,                       /* domainEventDeregister */
    NULL,                       /* domainMigratePrepare2 */
    NULL,                       /* domainMigrateFinish2 */
    NULL,                       /* nodeDeviceDettach */
    NULL,                       /* nodeDeviceReAttach */
    NULL,                       /* nodeDeviceReset */
    NULL,                       /* domainMigratePrepareTunnel */
    NULL,                       /* IsEncrypted */
    NULL,                       /* IsSecure */
    libxlDomainIsActive,        /* DomainIsActive */
    libxlDomainIsPersistent,    /* DomainIsPersistent */
    NULL,                       /* domainIsUpdated */
    NULL,                       /* cpuCompare */
    NULL,                       /* cpuBaseline */
    NULL,                       /* domainGetJobInfo */
    NULL,                       /* domainAbortJob */
    NULL,                       /* domainMigrateSetMaxDowntime */
    NULL,                       /* domainMigrateSetMaxSpeed */
    NULL,                       /* domainEventRegisterAny */
    NULL,                       /* domainEventDeregisterAny */
    NULL,                       /* domainManagedSave */
    NULL,                       /* domainHasManagedSaveImage */
    NULL,                       /* domainManagedSaveRemove */
    NULL,                       /* domainSnapshotCreateXML */
    NULL,                       /* domainSnapshotDumpXML */
    NULL,                       /* domainSnapshotNum */
    NULL,                       /* domainSnapshotListNames */
    NULL,                       /* domainSnapshotLookupByName */
    NULL,                       /* domainHasCurrentSnapshot */
    NULL,                       /* domainSnapshotCurrent */
    NULL,                       /* domainRevertToSnapshot */
    NULL,                       /* domainSnapshotDelete */
    NULL,                       /* qemuDomainMonitorCommand */
    NULL,                       /* domainOpenConsole */
};

static virStateDriver libxlStateDriver = {
    .name = "LIBXL",
    .initialize = libxlStartup,
    .cleanup = libxlShutdown,
    .reload = libxlReload,
    .active = libxlActive,
};


int
libxlRegister(void)
{
    if (virRegisterDriver(&libxlDriver) < 0)
        return -1;
    if (virRegisterStateDriver(&libxlStateDriver) < 0)
        return -1;

    return 0;
}