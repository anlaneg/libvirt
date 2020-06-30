/*
 * Copyright (C) 2010-2014 Red Hat, Inc.
 * Copyright IBM Corp. 2008
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
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "virnetdevveth.h"
#include "viralloc.h"
#include "virlog.h"
#include "vircommand.h"
#include "virerror.h"
#include "virfile.h"
#include "virstring.h"
#include "virnetdev.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("util.netdevveth");

/* Functions */

virMutex virNetDevVethCreateMutex = VIR_MUTEX_INITIALIZER;

static int virNetDevVethExists(int devNum)
{
    int ret;
    g_autofree char *path = NULL;

    path = g_strdup_printf(SYSFS_NET_DIR "vnet%d/", devNum);
    ret = virFileExists(path) ? 1 : 0;
    VIR_DEBUG("Checked dev vnet%d usage: %d", devNum, ret);
    return ret;
}

/**
 * virNetDevVethGetFreeNum:
 * @startDev: device number to start at (x in vethx)
 *
 * Looks in /sys/class/net/ to find the first available veth device
 * name.
 *
 * Returns non-negative device number on success or -1 in case of error
 */
static int virNetDevVethGetFreeNum(int startDev)
{
    int devNum;

#define MAX_DEV_NUM 65536

    for (devNum = startDev; devNum < MAX_DEV_NUM; devNum++) {
        int ret = virNetDevVethExists(devNum);
        if (ret < 0)
            return -1;
        if (ret == 0)
            return devNum;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("No free veth devices available"));
    return -1;
}

/**
 * virNetDevVethCreate:
 * @veth1: pointer to name for parent end of veth pair
 * @veth2: pointer to return name for container end of veth pair
 *
 * Creates a veth device pair using the ip command:
 * ip link add veth1 type veth peer name veth2
 * If veth1 points to NULL on entry, it will be a valid interface on
 * return.  veth2 should point to NULL on entry.
 *
 * NOTE: If veth1 and veth2 names are not specified, ip will auto assign
 *       names.  There seems to be two problems here -
 *       1) There doesn't seem to be a way to determine the names of the
 *          devices that it creates.  They show up in ip link show and
 *          under /sys/class/net/ however there is no guarantee that they
 *          are the devices that this process just created.
 *       2) Once one of the veth devices is moved to another namespace, it
 *          is no longer visible in the parent namespace.  This seems to
 *          confuse the name assignment causing it to fail with File exists.
 *       Because of these issues, this function currently allocates names
 *       prior to using the ip command, and returns any allocated names
 *       to the caller.
 *
 * Returns 0 on success or -1 in case of error
 */
int virNetDevVethCreate(char** veth1, char** veth2)
{
    int ret = -1;
    int vethNum = 0;
    size_t i;

    /*
     * We might race with other containers, but this is reasonably
     * unlikely, so don't do too many retries for device creation
     */
    virMutexLock(&virNetDevVethCreateMutex);
#define MAX_VETH_RETRIES 10

    for (i = 0; i < MAX_VETH_RETRIES; i++) {
        g_autofree char *veth1auto = NULL;
        g_autofree char *veth2auto = NULL;
        g_autoptr(virCommand) cmd = NULL;

        int status;
        if (!*veth1) {
        		//如果未给出veth1,则构造veth1对应的名称
            int veth1num;
            if ((veth1num = virNetDevVethGetFreeNum(vethNum)) < 0)
                goto cleanup;

            veth1auto = g_strdup_printf("vnet%d", veth1num);
            vethNum = veth1num + 1;
        }
        if (!*veth2) {
        		//如果未给出veth2,则构造veth2对应的名称
            int veth2num;
            if ((veth2num = virNetDevVethGetFreeNum(vethNum)) < 0)
                goto cleanup;

            veth2auto = g_strdup_printf("vnet%d", veth2num);
            vethNum = veth2num + 1;
        }

        //通过ip link添加veth类型的两个接口veth1,veth2
        cmd = virCommandNew("ip");
        virCommandAddArgList(cmd, "link", "add",
                             *veth1 ? *veth1 : veth1auto,
                             "type", "veth", "peer", "name",
                             *veth2 ? *veth2 : veth2auto,
                             NULL);

        if (virCommandRun(cmd, &status) < 0)
            goto cleanup;

        if (status == 0) {
            if (veth1auto) {
                *veth1 = veth1auto;
                veth1auto = NULL;
            }
            if (veth2auto) {
                *veth2 = veth2auto;
                veth2auto = NULL;
            }
            VIR_DEBUG("Create Host: %s guest: %s", *veth1, *veth2);
            ret = 0;
            goto cleanup;
        }

        VIR_DEBUG("Failed to create veth host: %s guest: %s: %d",
                  *veth1 ? *veth1 : veth1auto,
                  *veth2 ? *veth2 : veth2auto,
                  status);
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Failed to allocate free veth pair after %d attempts"),
                   MAX_VETH_RETRIES);

 cleanup:
    virMutexUnlock(&virNetDevVethCreateMutex);
    return ret;
}

/**
 * virNetDevVethDelete:
 * @veth: name for one end of veth pair
 *
 * This will delete both veth devices in a pair.  Only one end needs to
 * be specified.  The ip command will identify and delete the other veth
 * device as well.
 * ip link del veth
 *
 * Returns 0 on success or -1 in case of error
 */
int virNetDevVethDelete(const char *veth)
{
	//通过ip link delete删除veth接口
    int status;
    g_autoptr(virCommand) cmd = virCommandNewArgList("ip", "link",
                                                       "del", veth, NULL);

    if (virCommandRun(cmd, &status) < 0)
        return -1;

    if (status != 0) {
        if (!virNetDevExists(veth)) {
            VIR_DEBUG("Device %s already deleted (by kernel namespace cleanup)", veth);
            return 0;
        }
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to delete veth device %s"), veth);
        return -1;
    }

    return 0;
}
