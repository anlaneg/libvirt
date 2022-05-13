/*
 * virccw.c: helper APIs for managing host CCW devices
 *
 * Copyright (C) 2022 IBM Corporation
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
#include "virccw.h"


bool
virCCWDeviceAddressIsValid(virCCWDeviceAddress *addr)
{
    return addr->cssid <= VIR_CCW_DEVICE_MAX_CSSID &&
           addr->ssid <= VIR_CCW_DEVICE_MAX_SSID &&
           addr->devno <= VIR_CCW_DEVICE_MAX_DEVNO;
}

bool
virCCWDeviceAddressEqual(virCCWDeviceAddress *addr1,
                         virCCWDeviceAddress *addr2)
{
    if (addr1->cssid == addr2->cssid &&
        addr1->ssid == addr2->ssid &&
        addr1->devno == addr2->devno) {
        return true;
    }
    return false;
}

char*
virCCWDeviceAddressAsString(virCCWDeviceAddress *addr)
{
    return g_strdup_printf(VIR_CCW_DEVICE_ADDRESS_FMT, addr->cssid, addr->ssid, addr->devno);
}

int
virCCWDeviceAddressIncrement(virCCWDeviceAddress *addr)
{
    virCCWDeviceAddress ccwaddr = *addr;

    /* We are not touching subchannel sets and channel subsystems */
    if (++ccwaddr.devno > VIR_CCW_DEVICE_MAX_DEVNO)
        return -1;

    *addr = ccwaddr;
    return 0;
}
