/* $Id$ */
/** @file
 * VBox Qt GUI - UICloudNetworkingStuff namespace implementation.
 */

/*
 * Copyright (C) 2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* GUI includes: */
#include "UICloudNetworkingStuff.h"
#include "UICommon.h"
#include "UIMessageCenter.h"

/* COM includes: */
#include "CAppliance.h"
#include "CVirtualBox.h"
#include "CVirtualSystemDescription.h"


QString UICloudNetworkingStuff::getInstanceInfo(KVirtualSystemDescriptionType enmType,
                                                const CCloudClient &comCloudClient,
                                                const QString &strId,
                                                QWidget *pParent /* = 0 */)
{
    /* Get VirtualBox object: */
    CVirtualBox comVBox = uiCommon().virtualBox();

    /* Create appliance: */
    CAppliance comAppliance = comVBox.CreateAppliance();
    if (!comVBox.isOk())
    {
        if (pParent)
            msgCenter().cannotCreateAppliance(comVBox, pParent);
        else
        {
            /// @todo fetch error info
        }
    }
    else
    {
        /* Append it with one (1) description we need: */
        comAppliance.CreateVirtualSystemDescriptions(1);
        if (!comAppliance.isOk())
        {
            if (pParent)
                msgCenter().cannotCreateVirtualSystemDescription(comAppliance, pParent);
            else
            {
                /// @todo fetch error info
            }
        }
        else
        {
            /* Get received description: */
            QVector<CVirtualSystemDescription> descriptions = comAppliance.GetVirtualSystemDescriptions();
            AssertReturn(!descriptions.isEmpty(), QString());
            CVirtualSystemDescription comDescription = descriptions.at(0);

            /* Now execute GetInstanceInfo async method: */
            CProgress comProgress = comCloudClient.GetInstanceInfo(strId, comDescription);
            if (!comCloudClient.isOk())
            {
                if (pParent)
                    msgCenter().cannotAcquireCloudClientParameter(comCloudClient, pParent);
                else
                {
                    /// @todo fetch error info
                }
            }
            else
            {
                /* Show "Acquire instance info" progress: */
                if (pParent)
                    msgCenter().showModalProgressDialog(comProgress, UICommon::tr("Acquire instance info ..."),
                                                        ":/progress_reading_appliance_90px.png", pParent, 0);
                else
                    comProgress.WaitForCompletion(-1);
                if (!comProgress.isOk() || comProgress.GetResultCode() != 0)
                {
                    if (pParent)
                        msgCenter().cannotAcquireCloudClientParameter(comProgress, pParent);
                    else
                    {
                        /// @todo fetch error info
                    }
                }
                else
                {
                    /* Now acquire description of certain type: */
                    QVector<KVirtualSystemDescriptionType> types;
                    QVector<QString> refs, origValues, configValues, extraConfigValues;
                    comDescription.GetDescriptionByType(enmType, types, refs, origValues, configValues, extraConfigValues);

                    /* Return first config value if we have one: */
                    AssertReturn(!configValues.isEmpty(), QString());
                    return configValues.at(0);
                }
            }
        }
    }

    /* Return null string by default: */
    return QString();
}
