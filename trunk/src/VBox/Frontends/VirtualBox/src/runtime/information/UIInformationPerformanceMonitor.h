/* $Id$ */
/** @file
 * VBox Qt GUI - UIInformationPerformanceMonitor class declaration.
 */

/*
 * Copyright (C) 2016-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef FEQT_INCLUDED_SRC_runtime_information_UIInformationPerformanceMonitor_h
#define FEQT_INCLUDED_SRC_runtime_information_UIInformationPerformanceMonitor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* Qt includes: */
#include <QWidget>
#include <QMap>
#include <QQueue>


/* COM includes: */
#include "COMEnums.h"
#include "CConsole.h"
#include "CGuest.h"
#include "CMachine.h"
#include "CMachineDebugger.h"
#include "CPerformanceCollector.h"

/* GUI includes: */
#include "QIWithRetranslateUI.h"
#include "UIMainEventListener.h"


/* Forward declarations: */
class QTimer;
class QVBoxLayout;
class QLabel;
class UIChart;
class UISession;
class UIRuntimeInfoWidget;

#define DATA_SERIES_SIZE 2

struct DebuggerMetricData
{
    DebuggerMetricData()
        : m_counter(0){}
    DebuggerMetricData(const QString & strName, quint64 counter)
        :m_strName(strName)
        , m_counter(counter){}
    QString m_strName;
    quint64 m_counter;
};

class UIMetric
{
public:

    UIMetric(const QString &strName, const QString &strUnit, int iMaximumQueueSize);
    UIMetric();
    const QString &name() const;

    void setMaximum(quint64 iMaximum);
    quint64 maximum() const;

    void setUnit(QString strUnit);
    const QString &unit() const;

    void addData(int iDataSeriesIndex, quint64 fData);
    const QQueue<quint64> *data(int iDataSeriesIndex) const;

    void setTotal(int iDataSeriesIndex, quint64 iTotal);
    quint64 total(int iDataSeriesIndex) const;

    bool requiresGuestAdditions() const;
    void setRequiresGuestAdditions(bool fRequiresGAs);

    const QStringList &deviceTypeList() const;
    void setDeviceTypeList(const QStringList &list);

    void setQueryPrefix(const QString &strPrefix);

    const QStringList &metricDataSubString() const;
    void setMetricDataSubString(const QStringList &list);

    const QString &queryString() const;

    void setIsInitialized(bool fIsInitialized);
    bool isInitialized() const;

    void reset();

private:

    void composeQueryString();

    /** @name The following strings are string list are used while making IMachineDebugger::getStats calls and parsing the resultin
      * xml stream.
      * @{ */
        /** This string is used while calling IMachineDebugger::getStats(..). It is composed of
        * m_strQueryPrefix, m_deviceTypeList, and  m_metricDataSubString. */
        QString m_strQueryString;
        /** This list is used to differentiate xml data we get from the IMachineDebugger. */
        QStringList m_deviceTypeList;
        /** This is used to select data series of the metric. For example, for network metric
         * it is ReceiveBytes/TransmitBytes */
        QStringList m_metricDataSubString;
        QString m_strQueryPrefix;
    /** @} */

    QString m_strName;
    QString m_strUnit;
    quint64 m_iMaximum;
    QQueue<quint64> m_data[DATA_SERIES_SIZE];
    /** The total data (the counter value we get from IMachineDebugger API). For the metrics
      * we get from IMachineDebugger m_data values are computed as deltas of total values t - (t-1) */
    quint64 m_iTotal[DATA_SERIES_SIZE];
    int m_iMaximumQueueSize;
    bool m_fRequiresGuestAdditions;
    /** Used for metrices whose data is computed as total deltas. That is we receieve only total value and
      * compute time step data from total deltas. m_isInitialised is true if the total has been set first time. */
    bool m_fIsInitialized;
};

/** UIInformationPerformanceMonitor class displays some high level performance metric of the guest system.
  * The values are read in certain periods and cached in the GUI side. Currently we draw some line charts
  * and pie charts (where applicable) alongside with some text. Additionally it displays a table including some
  * run time attributes. */
class UIInformationPerformanceMonitor : public QIWithRetranslateUI<QWidget>
{
    Q_OBJECT;

public:

    /** Constructs information-tab passing @a pParent to the QWidget base-class constructor.
      * @param machine is machine reference.
      * @param console is machine console reference. */
    UIInformationPerformanceMonitor(QWidget *pParent, const CMachine &machine, const CConsole &console, const UISession *pSession);
    ~UIInformationPerformanceMonitor();

protected:
    void retranslateUi();

private slots:
    /** Reads the metric values for several sources and calls corresponding update functions. */
    void sltTimeout();
    /** @name These functions are connected to API events and implement necessary updates.
      * @{ */
        void sltGuestAdditionsStateChange();
    /** @} */

private:

    void prepareObjects();
    void prepareMetrics();
    bool guestAdditionsAvailable(int iMinimumMajorVersion);
    void enableDisableGuestAdditionDependedWidgets(bool fEnable);
    void updateCPUGraphsAndMetric(ULONG iLoadPercentage, ULONG iOtherPercentage);
    void updateRAMGraphsAndMetric(quint64 iTotalRAM, quint64 iFreeRAM);
    void updateNetworkGraphsAndMetric(quint64 iReceiveTotal, quint64 iTransmitTotal);
    void updateDiskIOGraphsAndMetric(quint64 uDiskIOTotalWritten, quint64 uDiskIOTotalRead);
    void updateVMExitMetric(quint64 uTotalVMExits);
    /** Returns a QColor for the chart with @p strChartName and data series with @p iDataIndex. */
    QString dataColorString(const QString &strChartName, int iDataIndex);
    /** Parses the xml string we get from the IMachineDebugger and returns an array of DebuggerMetricData. */
    QVector<DebuggerMetricData> getTotalCounterFromDegugger(const QString &strQuery);

    bool m_fGuestAdditionsAvailable;
    CMachine m_machine;
    CConsole m_console;
    CGuest m_comGuest;

    CPerformanceCollector m_performanceMonitor;
    CMachineDebugger      m_machineDebugger;
    /** Holds the instance of layout we create. */
    QVBoxLayout *m_pMainLayout;
    QTimer *m_pTimer;

    QVector<QString> m_nameList;
    QVector<CUnknown> m_objectList;

    QMap<QString, UIMetric> m_subMetrics;
    QMap<QString,UIChart*>  m_charts;
    QMap<QString,QLabel*>  m_infoLabels;
    ComObjPtr<UIMainEventListenerImpl> m_pQtGuestListener;

    /** @name These metric names are used for map keys to identify metrics.
      * @{ */
        QString m_strCPUMetricName;
        QString m_strRAMMetricName;
        QString m_strDiskMetricName;
        QString m_strNetworkMetricName;
        QString m_strDiskIOMetricName;
        QString m_strVMExitMetricName;
    /** @} */

    /** @name Cached translated strings.
      * @{ */
        /** CPU info label strings. */
        QString m_strCPUInfoLabelTitle;
        QString m_strCPUInfoLabelGuest;
        QString  m_strCPUInfoLabelVMM;
        /** RAM usage info label strings. */
        QString m_strRAMInfoLabelTitle;
        QString m_strRAMInfoLabelTotal;
        QString m_strRAMInfoLabelFree;
        QString m_strRAMInfoLabelUsed;
        /** Net traffic info label strings. */
        QString m_strNetworkInfoLabelTitle;
        QString m_strNetworkInfoLabelReceived;
        QString m_strNetworkInfoLabelTransmitted;
        QString m_strNetworkInfoLabelReceivedTotal;
        QString m_strNetworkInfoLabelTransmittedTotal;
        /** Disk IO info label strings. */
        QString m_strDiskIOInfoLabelTitle;
        QString m_strDiskIOInfoLabelWritten;
        QString m_strDiskIOInfoLabelRead;
        QString m_strDiskIOInfoLabelWrittenTotal;
        QString m_strDiskIOInfoLabelReadTotal;
        /** VM Exit info label strings. */
        QString m_strVMExitInfoLabelTitle;
        QString m_strVMExitLabelCurrent;
        QString m_strVMExitLabelTotal;
    /** @} */
    /** The following string is used while querrying CMachineDebugger. */
    QString m_strQueryString;
    quint64 m_iTimeStep;
};

#endif /* !FEQT_INCLUDED_SRC_runtime_information_UIInformationPerformanceMonitor_h */