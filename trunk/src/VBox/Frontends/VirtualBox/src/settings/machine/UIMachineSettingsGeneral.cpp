/* $Id$ */
/** @file
 * VBox Qt GUI - UIMachineSettingsGeneral class implementation.
 */

/*
 * Copyright (C) 2006-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>

/* GUI includes: */
#include "QITabWidget.h"
#include "QIWidgetValidator.h"
#include "UICommon.h"
#include "UIConverter.h"
#include "UIErrorString.h"
#include "UIFilePathSelector.h"
#include "UIMachineSettingsGeneral.h"
#include "UIModalWindowManager.h"
#include "UINameAndSystemEditor.h"
#include "UIProgressDialog.h"

/* COM includes: */
#include "CExtPack.h"
#include "CExtPackManager.h"
#include "CMedium.h"
#include "CMediumAttachment.h"
#include "CProgress.h"


/** Machine settings: General page data structure. */
struct UIDataSettingsMachineGeneral
{
    /** Constructs data. */
    UIDataSettingsMachineGeneral()
        : m_strName(QString())
        , m_strGuestOsTypeId(QString())
        , m_strSnapshotsFolder(QString())
        , m_strSnapshotsHomeDir(QString())
        , m_clipboardMode(KClipboardMode_Disabled)
        , m_dndMode(KDnDMode_Disabled)
        , m_strDescription(QString())
        , m_fEncryptionEnabled(false)
        , m_fEncryptionCipherChanged(false)
        , m_fEncryptionPasswordChanged(false)
        , m_iEncryptionCipherIndex(-1)
        , m_strEncryptionPassword(QString())
    {}

    /** Returns whether the @a other passed data is equal to this one. */
    bool equal(const UIDataSettingsMachineGeneral &other) const
    {
        return true
               && (m_strName == other.m_strName)
               && (m_strGuestOsTypeId == other.m_strGuestOsTypeId)
               && (m_strSnapshotsFolder == other.m_strSnapshotsFolder)
               && (m_strSnapshotsHomeDir == other.m_strSnapshotsHomeDir)
               && (m_clipboardMode == other.m_clipboardMode)
               && (m_dndMode == other.m_dndMode)
               && (m_strDescription == other.m_strDescription)
               && (m_fEncryptionEnabled == other.m_fEncryptionEnabled)
               && (m_fEncryptionCipherChanged == other.m_fEncryptionCipherChanged)
               && (m_fEncryptionPasswordChanged == other.m_fEncryptionPasswordChanged)
               ;
    }

    /** Returns whether the @a other passed data is equal to this one. */
    bool operator==(const UIDataSettingsMachineGeneral &other) const { return equal(other); }
    /** Returns whether the @a other passed data is different from this one. */
    bool operator!=(const UIDataSettingsMachineGeneral &other) const { return !equal(other); }

    /** Holds the VM name. */
    QString  m_strName;
    /** Holds the VM OS type ID. */
    QString  m_strGuestOsTypeId;

    /** Holds the VM snapshot folder. */
    QString         m_strSnapshotsFolder;
    /** Holds the default VM snapshot folder. */
    QString         m_strSnapshotsHomeDir;
    /** Holds the VM clipboard mode. */
    KClipboardMode  m_clipboardMode;
    /** Holds the VM drag&drop mode. */
    KDnDMode        m_dndMode;

    /** Holds the VM description. */
    QString  m_strDescription;

    /** Holds whether the encryption is enabled. */
    bool                   m_fEncryptionEnabled;
    /** Holds whether the encryption cipher was changed. */
    bool                   m_fEncryptionCipherChanged;
    /** Holds whether the encryption password was changed. */
    bool                   m_fEncryptionPasswordChanged;
    /** Holds the encryption cipher index. */
    int                    m_iEncryptionCipherIndex;
    /** Holds the encryption password. */
    QString                m_strEncryptionPassword;
    /** Holds the encrypted medium ids. */
    EncryptedMediumMap     m_encryptedMedia;
    /** Holds the encryption passwords. */
    EncryptionPasswordMap  m_encryptionPasswords;
};


UIMachineSettingsGeneral::UIMachineSettingsGeneral()
    : m_fHWVirtExEnabled(false)
    , m_fEncryptionCipherChanged(false)
    , m_fEncryptionPasswordChanged(false)
    , m_pCache(0)
    , m_pNameAndSystemEditor(0)
    , mPsSnapshot(0)
    , mCbClipboard(0)
    , m_pComboCipher(0)
    , mCbDragAndDrop(0)
    , mTeDescription(0)
    , m_pEditorEncryptionPassword(0)
    , m_pEditorEncryptionPasswordConfirm(0)
    , m_pCheckBoxEncryption(0)
    , m_pTabWidgetGeneral(0)
    , m_pTabBasic(0)
    , m_pTabDescription(0)
    , m_pTabAdvanced(0)
    , m_pTabEncryption(0)
    , m_pWidgetEncryption(0)
    , m_pLabelDragAndDrop(0)
    , m_pLabelCipher(0)
    , m_pLabelSnapshot(0)
    , m_pLabelClipboard(0)
    , m_pLabelPassword1(0)
    , m_pLabelPassword2(0)
{
    /* Prepare: */
    prepare();
}

UIMachineSettingsGeneral::~UIMachineSettingsGeneral()
{
    /* Cleanup: */
    cleanup();
}

CGuestOSType UIMachineSettingsGeneral::guestOSType() const
{
    AssertPtrReturn(m_pNameAndSystemEditor, CGuestOSType());
    return m_pNameAndSystemEditor->type();
}

bool UIMachineSettingsGeneral::is64BitOSTypeSelected() const
{
    AssertPtrReturn(m_pNameAndSystemEditor, false);
    return   m_pNameAndSystemEditor->type().isNotNull()
           ? m_pNameAndSystemEditor->type().GetIs64Bit()
           : false;
}

void UIMachineSettingsGeneral::setHWVirtExEnabled(bool fEnabled)
{
    /* Make sure hardware virtualization extension has changed: */
    if (m_fHWVirtExEnabled == fEnabled)
        return;

    /* Update hardware virtualization extension value: */
    m_fHWVirtExEnabled = fEnabled;

    /* Revalidate: */
    revalidate();
}

bool UIMachineSettingsGeneral::changed() const
{
    return m_pCache->wasChanged();
}

void UIMachineSettingsGeneral::loadToCacheFrom(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Clear cache initially: */
    m_pCache->clear();

    /* Prepare old general data: */
    UIDataSettingsMachineGeneral oldGeneralData;

    /* Gather old 'Basic' data: */
    oldGeneralData.m_strName = m_machine.GetName();
    oldGeneralData.m_strGuestOsTypeId = m_machine.GetOSTypeId();

    /* Gather old 'Advanced' data: */
    oldGeneralData.m_strSnapshotsFolder = m_machine.GetSnapshotFolder();
    oldGeneralData.m_strSnapshotsHomeDir = QFileInfo(m_machine.GetSettingsFilePath()).absolutePath();
    oldGeneralData.m_clipboardMode = m_machine.GetClipboardMode();
    oldGeneralData.m_dndMode = m_machine.GetDnDMode();

    /* Gather old 'Description' data: */
    oldGeneralData.m_strDescription = m_machine.GetDescription();

    /* Gather old 'Encryption' data: */
    QString strCipher;
    bool fEncryptionCipherCommon = true;
    /* Prepare the map of the encrypted media: */
    EncryptedMediumMap encryptedMedia;
    foreach (const CMediumAttachment &attachment, m_machine.GetMediumAttachments())
    {
        /* Check hard-drive attachments only: */
        if (attachment.GetType() == KDeviceType_HardDisk)
        {
            /* Get the attachment medium base: */
            const CMedium comMedium = attachment.GetMedium();
            /* Check medium encryption attributes: */
            QString strCurrentCipher;
            const QString strCurrentPasswordId = comMedium.GetEncryptionSettings(strCurrentCipher);
            if (comMedium.isOk())
            {
                encryptedMedia.insert(strCurrentPasswordId, comMedium.GetId());
                if (strCurrentCipher != strCipher)
                {
                    if (strCipher.isNull())
                        strCipher = strCurrentCipher;
                    else
                        fEncryptionCipherCommon = false;
                }
            }
        }
    }
    oldGeneralData.m_fEncryptionEnabled = !encryptedMedia.isEmpty();
    oldGeneralData.m_fEncryptionCipherChanged = false;
    oldGeneralData.m_fEncryptionPasswordChanged = false;
    if (fEncryptionCipherCommon)
        oldGeneralData.m_iEncryptionCipherIndex = m_encryptionCiphers.indexOf(strCipher);
    if (oldGeneralData.m_iEncryptionCipherIndex == -1)
        oldGeneralData.m_iEncryptionCipherIndex = 0;
    oldGeneralData.m_encryptedMedia = encryptedMedia;

    /* Cache old general data: */
    m_pCache->cacheInitialData(oldGeneralData);

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

void UIMachineSettingsGeneral::getFromCache()
{
    /* Get old general data from the cache: */
    const UIDataSettingsMachineGeneral &oldGeneralData = m_pCache->base();

    /* We are doing that *now* because these combos have
     * dynamical content which depends on cashed value: */
    repopulateComboClipboardMode();
    repopulateComboDnDMode();

    /* Load old 'Basic' data from the cache: */
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    m_pNameAndSystemEditor->setName(oldGeneralData.m_strName);
    m_pNameAndSystemEditor->setTypeId(oldGeneralData.m_strGuestOsTypeId);

    /* Load old 'Advanced' data from the cache: */
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(mCbDragAndDrop);
    mPsSnapshot->setPath(oldGeneralData.m_strSnapshotsFolder);
    mPsSnapshot->setHomeDir(oldGeneralData.m_strSnapshotsHomeDir);
    const int iClipboardModePosition = mCbClipboard->findData(oldGeneralData.m_clipboardMode);
    mCbClipboard->setCurrentIndex(iClipboardModePosition == -1 ? 0 : iClipboardModePosition);
    const int iDnDModePosition = mCbDragAndDrop->findData(oldGeneralData.m_dndMode);
    mCbDragAndDrop->setCurrentIndex(iDnDModePosition == -1 ? 0 : iDnDModePosition);

    /* Load old 'Description' data from the cache: */
    AssertPtrReturnVoid(mTeDescription);
    mTeDescription->setPlainText(oldGeneralData.m_strDescription);

    /* Load old 'Encryption' data from the cache: */
    AssertPtrReturnVoid(m_pCheckBoxEncryption);
    AssertPtrReturnVoid(m_pComboCipher);
    m_pCheckBoxEncryption->setChecked(oldGeneralData.m_fEncryptionEnabled);
    m_pComboCipher->setCurrentIndex(oldGeneralData.m_iEncryptionCipherIndex);
    m_fEncryptionCipherChanged = oldGeneralData.m_fEncryptionCipherChanged;
    m_fEncryptionPasswordChanged = oldGeneralData.m_fEncryptionPasswordChanged;

    /* Polish page finally: */
    polishPage();

    /* Revalidate: */
    revalidate();
}

void UIMachineSettingsGeneral::putToCache()
{
    /* Prepare new general data: */
    UIDataSettingsMachineGeneral newGeneralData;

    /* Gather new 'Basic' data: */
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    newGeneralData.m_strName = m_pNameAndSystemEditor->name();
    newGeneralData.m_strGuestOsTypeId = m_pNameAndSystemEditor->typeId();

    /* Gather new 'Advanced' data: */
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(mCbDragAndDrop);
    newGeneralData.m_strSnapshotsFolder = mPsSnapshot->path();
    newGeneralData.m_clipboardMode = mCbClipboard->currentData().value<KClipboardMode>();
    newGeneralData.m_dndMode = mCbDragAndDrop->currentData().value<KDnDMode>();

    /* Gather new 'Description' data: */
    AssertPtrReturnVoid(mTeDescription);
    newGeneralData.m_strDescription = mTeDescription->toPlainText().isEmpty() ?
                                      QString::null : mTeDescription->toPlainText();

    /* Gather new 'Encryption' data: */
    AssertPtrReturnVoid(m_pCheckBoxEncryption);
    AssertPtrReturnVoid(m_pComboCipher);
    AssertPtrReturnVoid(m_pEditorEncryptionPassword);
    newGeneralData.m_fEncryptionEnabled = m_pCheckBoxEncryption->isChecked();
    newGeneralData.m_fEncryptionCipherChanged = m_fEncryptionCipherChanged;
    newGeneralData.m_fEncryptionPasswordChanged = m_fEncryptionPasswordChanged;
    newGeneralData.m_iEncryptionCipherIndex = m_pComboCipher->currentIndex();
    newGeneralData.m_strEncryptionPassword = m_pEditorEncryptionPassword->text();
    newGeneralData.m_encryptedMedia = m_pCache->base().m_encryptedMedia;
    /* If encryption status, cipher or password is changed: */
    if (newGeneralData.m_fEncryptionEnabled != m_pCache->base().m_fEncryptionEnabled ||
        newGeneralData.m_fEncryptionCipherChanged != m_pCache->base().m_fEncryptionCipherChanged ||
        newGeneralData.m_fEncryptionPasswordChanged != m_pCache->base().m_fEncryptionPasswordChanged)
    {
        /* Ask for the disk encryption passwords if necessary: */
        if (!m_pCache->base().m_encryptedMedia.isEmpty())
        {
            /* Create corresponding dialog: */
            QWidget *pDlgParent = windowManager().realParentWindow(window());
            QPointer<UIAddDiskEncryptionPasswordDialog> pDlg =
                 new UIAddDiskEncryptionPasswordDialog(pDlgParent,
                                                       newGeneralData.m_strName,
                                                       newGeneralData.m_encryptedMedia);
            /* Execute it and acquire the result: */
            if (pDlg->exec() == QDialog::Accepted)
                newGeneralData.m_encryptionPasswords = pDlg->encryptionPasswords();
            /* Delete dialog if still valid: */
            if (pDlg)
                delete pDlg;
        }
    }

    /* Cache new general data: */
    m_pCache->cacheCurrentData(newGeneralData);
}

void UIMachineSettingsGeneral::saveFromCacheTo(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Update general data and failing state: */
    setFailed(!saveGeneralData());

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

bool UIMachineSettingsGeneral::validate(QList<UIValidationMessage> &messages)
{
    /* Pass by default: */
    bool fPass = true;

    /* Prepare message: */
    UIValidationMessage message;

    /* 'Basic' tab validations: */
    message.first = UICommon::removeAccelMark(m_pTabWidgetGeneral->tabText(0));
    message.second.clear();

    /* VM name validation: */
    AssertPtrReturn(m_pNameAndSystemEditor, false);
    if (m_pNameAndSystemEditor->name().trimmed().isEmpty())
    {
        message.second << tr("No name specified for the virtual machine.");
        fPass = false;
    }

    /* OS type & VT-x/AMD-v correlation: */
    if (is64BitOSTypeSelected() && !m_fHWVirtExEnabled)
    {
        message.second << tr("The virtual machine operating system hint is set to a 64-bit type. "
                             "64-bit guest systems require hardware virtualization, "
                             "so this will be enabled automatically if you confirm the changes.");
    }

    /* Serialize message: */
    if (!message.second.isEmpty())
        messages << message;

    /* 'Encryption' tab validations: */
    message.first = UICommon::removeAccelMark(m_pTabWidgetGeneral->tabText(3));
    message.second.clear();

    /* Encryption validation: */
    AssertPtrReturn(m_pCheckBoxEncryption, false);
    if (m_pCheckBoxEncryption->isChecked())
    {
#ifdef VBOX_WITH_EXTPACK
        /* Encryption Extension Pack presence test: */
        const CExtPack extPack = uiCommon().virtualBox().GetExtensionPackManager().Find(GUI_ExtPackName);
        if (extPack.isNull() || !extPack.GetUsable())
        {
            message.second << tr("You are trying to enable disk encryption for this virtual machine. "
                                 "However, this requires the <i>%1</i> to be installed. "
                                 "Please install the Extension Pack from the VirtualBox download site.")
                                 .arg(GUI_ExtPackName);
            fPass = false;
        }
#endif /* VBOX_WITH_EXTPACK */

        /* Cipher should be chosen if once changed: */
        AssertPtrReturn(m_pComboCipher, false);
        if (!m_pCache->base().m_fEncryptionEnabled ||
            m_fEncryptionCipherChanged)
        {
            if (m_pComboCipher->currentIndex() == 0)
                message.second << tr("Disk encryption cipher type not specified.");
            fPass = false;
        }

        /* Password should be entered and confirmed if once changed: */
        AssertPtrReturn(m_pEditorEncryptionPassword, false);
        AssertPtrReturn(m_pEditorEncryptionPasswordConfirm, false);
        if (!m_pCache->base().m_fEncryptionEnabled ||
            m_fEncryptionPasswordChanged)
        {
            if (m_pEditorEncryptionPassword->text().isEmpty())
                message.second << tr("Disk encryption password empty.");
            else
            if (m_pEditorEncryptionPassword->text() !=
                m_pEditorEncryptionPasswordConfirm->text())
                message.second << tr("Disk encryption passwords do not match.");
            fPass = false;
        }
    }

    /* Serialize message: */
    if (!message.second.isEmpty())
        messages << message;

    /* Return result: */
    return fPass;
}

void UIMachineSettingsGeneral::setOrderAfter(QWidget *pWidget)
{
    /* 'Basic' tab: */
    AssertPtrReturnVoid(pWidget);
    AssertPtrReturnVoid(m_pTabWidgetGeneral);
    AssertPtrReturnVoid(m_pTabWidgetGeneral->focusProxy());
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    setTabOrder(pWidget, m_pTabWidgetGeneral->focusProxy());
    setTabOrder(m_pTabWidgetGeneral->focusProxy(), m_pNameAndSystemEditor);

    /* 'Advanced' tab: */
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(mCbDragAndDrop);
    setTabOrder(m_pNameAndSystemEditor, mPsSnapshot);
    setTabOrder(mPsSnapshot, mCbClipboard);
    setTabOrder(mCbClipboard, mCbDragAndDrop);

    /* 'Description' tab: */
    AssertPtrReturnVoid(mTeDescription);
    setTabOrder(mCbDragAndDrop, mTeDescription);
}

void UIMachineSettingsGeneral::retranslateUi()
{
    m_pTabWidgetGeneral->setTabText(m_pTabWidgetGeneral->indexOf(m_pTabBasic), QApplication::translate("UIMachineSettingsGeneral", "Basi&c"));
    m_pLabelSnapshot->setText(QApplication::translate("UIMachineSettingsGeneral", "S&napshot Folder:"));
    m_pLabelClipboard->setText(QApplication::translate("UIMachineSettingsGeneral", "&Shared Clipboard:"));
    mCbClipboard->setWhatsThis(QApplication::translate("UIMachineSettingsGeneral", "Selects which clipboard data will be copied "
                                                       "between the guest and the host OS. This feature requires Guest Additions "
                                                       "to be installed in the guest OS."));
    m_pLabelDragAndDrop->setText(QApplication::translate("UIMachineSettingsGeneral", "D&rag'n'Drop:"));
    mCbDragAndDrop->setWhatsThis(QApplication::translate("UIMachineSettingsGeneral", "Selects which data will be copied between "
                                                         "the guest and the host OS by drag'n'drop. This feature requires Guest "
                                                         "Additions to be installed in the guest OS."));
    m_pTabWidgetGeneral->setTabText(m_pTabWidgetGeneral->indexOf(m_pTabAdvanced), QApplication::translate("UIMachineSettingsGeneral", "A&dvanced"));
    mTeDescription->setWhatsThis(QApplication::translate("UIMachineSettingsGeneral", "Holds the description of the virtual machine. "
                                                         "The description field is useful for commenting on configuration details "
                                                         "of the installed guest OS."));
    m_pTabWidgetGeneral->setTabText(m_pTabWidgetGeneral->indexOf(m_pTabDescription), QApplication::translate("UIMachineSettingsGeneral", "D&escription"));
    m_pCheckBoxEncryption->setWhatsThis(QApplication::translate("UIMachineSettingsGeneral", "When checked, disks attached to this "
                                                                "virtual machine will be encrypted."));
    m_pCheckBoxEncryption->setText(QApplication::translate("UIMachineSettingsGeneral", "En&able Disk Encryption"));
    m_pLabelCipher->setText(QApplication::translate("UIMachineSettingsGeneral", "Disk Encryption C&ipher:"));
    m_pComboCipher->setWhatsThis(QApplication::translate("UIMachineSettingsGeneral", "Selects the cipher to be used for encrypting "
                                                         "the virtual machine disks."));
    m_pLabelPassword1->setText(QApplication::translate("UIMachineSettingsGeneral", "E&nter New Password:"));
    m_pEditorEncryptionPassword->setWhatsThis(QApplication::translate("UIMachineSettingsGeneral", "Holds the encryption password "
                                                                      "for disks attached to this virtual machine."));
    m_pLabelPassword2->setText(QApplication::translate("UIMachineSettingsGeneral", "C&onfirm New Password:"));
    m_pEditorEncryptionPasswordConfirm->setWhatsThis(QApplication::translate("UIMachineSettingsGeneral", "Confirms the disk encryption password."));
    m_pTabWidgetGeneral->setTabText(m_pTabWidgetGeneral->indexOf(m_pTabEncryption), QApplication::translate("UIMachineSettingsGeneral", "Disk Enc&ryption"));

    /* Translate path selector: */
    AssertPtrReturnVoid(mPsSnapshot);
    mPsSnapshot->setWhatsThis(tr("Holds the path where snapshots of this "
                                 "virtual machine will be stored. Be aware that "
                                 "snapshots can take quite a lot of storage space."));

    /* Translate Clipboard mode combo: */
    AssertPtrReturnVoid(mCbClipboard);
    for (int iIndex = 0; iIndex < mCbClipboard->count(); ++iIndex)
    {
        const KClipboardMode enmType = mCbClipboard->currentData().value<KClipboardMode>();
        mCbClipboard->setItemText(iIndex, gpConverter->toString(enmType));
    }

    /* Translate Drag'n'drop mode combo: */
    AssertPtrReturnVoid(mCbDragAndDrop);
    for (int iIndex = 0; iIndex < mCbDragAndDrop->count(); ++iIndex)
    {
        const KDnDMode enmType = mCbDragAndDrop->currentData().value<KDnDMode>();
        mCbDragAndDrop->setItemText(iIndex, gpConverter->toString(enmType));
    }

    /* Translate Cipher type combo: */
    AssertPtrReturnVoid(m_pComboCipher);
    m_pComboCipher->setItemText(0, tr("Leave Unchanged", "cipher type"));
}

void UIMachineSettingsGeneral::polishPage()
{
    /* Polish 'Basic' availability: */
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    m_pNameAndSystemEditor->setNameStuffEnabled(isMachineOffline() || isMachineSaved());
    m_pNameAndSystemEditor->setPathStuffEnabled(isMachineOffline());
    m_pNameAndSystemEditor->setOSTypeStuffEnabled(isMachineOffline());

    /* Polish 'Advanced' availability: */
    AssertPtrReturnVoid(m_pLabelSnapshot);
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(m_pLabelClipboard);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(m_pLabelDragAndDrop);
    AssertPtrReturnVoid(mCbDragAndDrop);
    m_pLabelSnapshot->setEnabled(isMachineOffline());
    mPsSnapshot->setEnabled(isMachineOffline());
    m_pLabelClipboard->setEnabled(isMachineInValidMode());
    mCbClipboard->setEnabled(isMachineInValidMode());
    m_pLabelDragAndDrop->setEnabled(isMachineInValidMode());
    mCbDragAndDrop->setEnabled(isMachineInValidMode());

    /* Polish 'Description' availability: */
    AssertPtrReturnVoid(mTeDescription);
    mTeDescription->setEnabled(isMachineInValidMode());

    /* Polish 'Encryption' availability: */
    AssertPtrReturnVoid(m_pCheckBoxEncryption);
    AssertPtrReturnVoid(m_pWidgetEncryption);
    m_pCheckBoxEncryption->setEnabled(isMachineOffline());
    m_pWidgetEncryption->setEnabled(isMachineOffline() && m_pCheckBoxEncryption->isChecked());
}

void UIMachineSettingsGeneral::prepare()
{
    prepareWidgets();

    /* Prepare cache: */
    m_pCache = new UISettingsCacheMachineGeneral;
    AssertPtrReturnVoid(m_pCache);

    /* Tree-widget created in the .ui file. */
    {
        /* Prepare 'Basic' tab: */
        prepareTabBasic();
        /* Prepare 'Description' tab: */
        prepareTabDescription();
        /* Prepare 'Encryption' tab: */
        prepareTabEncryption();
        /* Prepare connections: */
        prepareConnections();
    }

    /* Apply language settings: */
    retranslateUi();
}

void UIMachineSettingsGeneral::prepareWidgets()
{
    QHBoxLayout *mLtMain;


    QVBoxLayout *mLtBasic;

    QSpacerItem *mSpVer1;

    QVBoxLayout *mLtAdvanced;
    QWidget *mWtAdvanced;
    QGridLayout *mLtAdvancedItems;




    QSpacerItem *mSpHor1;


    QSpacerItem *mSpHor2;
    QSpacerItem *mSpVer3;

    QVBoxLayout *mLtDescription;
    QGridLayout *m_pLayoutEncryption;

    QSpacerItem *spacerItem;
    QGridLayout *m_pLayoutEncryptionSettings;


    QSpacerItem *spacerItem1;

    if (objectName().isEmpty())
        setObjectName(QStringLiteral("UIMachineSettingsGeneral"));
    resize(350, 250);
    mLtMain = new QHBoxLayout(this);
    mLtMain->setObjectName(QStringLiteral("mLtMain"));
    m_pTabWidgetGeneral = new QITabWidget();
    m_pTabWidgetGeneral->setObjectName(QStringLiteral("m_pTabWidgetGeneral"));
    m_pTabBasic = new QWidget();
    m_pTabBasic->setObjectName(QStringLiteral("m_pTabBasic"));
    mLtBasic = new QVBoxLayout(m_pTabBasic);
    mLtBasic->setSpacing(0);
    mLtBasic->setObjectName(QStringLiteral("mLtBasic"));
    m_pNameAndSystemEditor = new UINameAndSystemEditor(m_pTabBasic);
    m_pNameAndSystemEditor->setObjectName(QStringLiteral("m_pNameAndSystemEditor"));

    mLtBasic->addWidget(m_pNameAndSystemEditor);

    mSpVer1 = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);

    mLtBasic->addItem(mSpVer1);

    m_pTabWidgetGeneral->addTab(m_pTabBasic, QString());
    m_pTabAdvanced = new QWidget();
    m_pTabAdvanced->setObjectName(QStringLiteral("m_pTabAdvanced"));
    mLtAdvanced = new QVBoxLayout(m_pTabAdvanced);
    mLtAdvanced->setSpacing(0);
    mLtAdvanced->setObjectName(QStringLiteral("mLtAdvanced"));
    mWtAdvanced = new QWidget(m_pTabAdvanced);
    mWtAdvanced->setObjectName(QStringLiteral("mWtAdvanced"));
    mLtAdvancedItems = new QGridLayout(mWtAdvanced);
    mLtAdvancedItems->setContentsMargins(0, 0, 0, 0);
    mLtAdvancedItems->setObjectName(QStringLiteral("mLtAdvancedItems"));
    m_pLabelSnapshot = new QLabel(mWtAdvanced);
    m_pLabelSnapshot->setObjectName(QStringLiteral("m_pLabelSnapshot"));
    m_pLabelSnapshot->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

    mLtAdvancedItems->addWidget(m_pLabelSnapshot, 0, 0, 1, 1);

    mPsSnapshot = new UIFilePathSelector(mWtAdvanced);
    mPsSnapshot->setObjectName(QStringLiteral("mPsSnapshot"));
    QSizePolicy sizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    sizePolicy.setHorizontalStretch(0);
    sizePolicy.setVerticalStretch(0);
    sizePolicy.setHeightForWidth(mPsSnapshot->sizePolicy().hasHeightForWidth());
    mPsSnapshot->setSizePolicy(sizePolicy);

    mLtAdvancedItems->addWidget(mPsSnapshot, 0, 1, 1, 2);

    m_pLabelClipboard = new QLabel(mWtAdvanced);
    m_pLabelClipboard->setObjectName(QStringLiteral("m_pLabelClipboard"));
    m_pLabelClipboard->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

    mLtAdvancedItems->addWidget(m_pLabelClipboard, 1, 0, 1, 1);

    mCbClipboard = new QComboBox(mWtAdvanced);
    mCbClipboard->setObjectName(QStringLiteral("mCbClipboard"));
    QSizePolicy sizePolicy1(QSizePolicy::Fixed, QSizePolicy::Fixed);
    sizePolicy1.setHorizontalStretch(0);
    sizePolicy1.setVerticalStretch(0);
    sizePolicy1.setHeightForWidth(mCbClipboard->sizePolicy().hasHeightForWidth());
    mCbClipboard->setSizePolicy(sizePolicy1);

    mLtAdvancedItems->addWidget(mCbClipboard, 1, 1, 1, 1);

    mSpHor1 = new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);

    mLtAdvancedItems->addItem(mSpHor1, 1, 2, 1, 1);

    m_pLabelDragAndDrop = new QLabel(mWtAdvanced);
    m_pLabelDragAndDrop->setObjectName(QStringLiteral("m_pLabelDragAndDrop"));
    m_pLabelDragAndDrop->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

    mLtAdvancedItems->addWidget(m_pLabelDragAndDrop, 2, 0, 1, 1);

    mCbDragAndDrop = new QComboBox(mWtAdvanced);
    mCbDragAndDrop->setObjectName(QStringLiteral("mCbDragAndDrop"));
    sizePolicy1.setHeightForWidth(mCbDragAndDrop->sizePolicy().hasHeightForWidth());
    mCbDragAndDrop->setSizePolicy(sizePolicy1);

    mLtAdvancedItems->addWidget(mCbDragAndDrop, 2, 1, 1, 1);

    mSpHor2 = new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);

    mLtAdvancedItems->addItem(mSpHor2, 2, 2, 1, 1);


    mLtAdvanced->addWidget(mWtAdvanced);

    mSpVer3 = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);

    mLtAdvanced->addItem(mSpVer3);

    m_pTabWidgetGeneral->addTab(m_pTabAdvanced, QString());
    m_pTabDescription = new QWidget();
    m_pTabDescription->setObjectName(QStringLiteral("m_pTabDescription"));
    mLtDescription = new QVBoxLayout(m_pTabDescription);
    mLtDescription->setSpacing(0);
    mLtDescription->setObjectName(QStringLiteral("mLtDescription"));
    mTeDescription = new QTextEdit(m_pTabDescription);
    mTeDescription->setObjectName(QStringLiteral("mTeDescription"));
    mTeDescription->setAcceptRichText(false);

    mLtDescription->addWidget(mTeDescription);

    m_pTabWidgetGeneral->addTab(m_pTabDescription, QString());
    m_pTabEncryption = new QWidget();
    m_pTabEncryption->setObjectName(QStringLiteral("m_pTabEncryption"));
    m_pLayoutEncryption = new QGridLayout(m_pTabEncryption);
    m_pLayoutEncryption->setObjectName(QStringLiteral("m_pLayoutEncryption"));
    m_pCheckBoxEncryption = new QCheckBox(m_pTabEncryption);
    m_pCheckBoxEncryption->setObjectName(QStringLiteral("m_pCheckBoxEncryption"));

    m_pLayoutEncryption->addWidget(m_pCheckBoxEncryption, 0, 0, 1, 2);

    spacerItem = new QSpacerItem(20, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);

    m_pLayoutEncryption->addItem(spacerItem, 1, 0, 1, 1);

    m_pWidgetEncryption = new QWidget(m_pTabEncryption);
    m_pWidgetEncryption->setObjectName(QStringLiteral("m_pWidgetEncryption"));
    QSizePolicy sizePolicy2(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    sizePolicy2.setHorizontalStretch(1);
    sizePolicy2.setVerticalStretch(0);
    sizePolicy2.setHeightForWidth(m_pWidgetEncryption->sizePolicy().hasHeightForWidth());
    m_pWidgetEncryption->setSizePolicy(sizePolicy2);
    m_pLayoutEncryptionSettings = new QGridLayout(m_pWidgetEncryption);
    m_pLayoutEncryptionSettings->setObjectName(QStringLiteral("m_pLayoutEncryptionSettings"));
    m_pLayoutEncryptionSettings->setContentsMargins(0, 0, 0, 0);
    m_pLabelCipher = new QLabel(m_pWidgetEncryption);
    m_pLabelCipher->setObjectName(QStringLiteral("m_pLabelCipher"));
    m_pLabelCipher->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

    m_pLayoutEncryptionSettings->addWidget(m_pLabelCipher, 0, 0, 1, 1);

    m_pComboCipher = new QComboBox(m_pWidgetEncryption);
    m_pComboCipher->setObjectName(QStringLiteral("m_pComboCipher"));

    m_pLayoutEncryptionSettings->addWidget(m_pComboCipher, 0, 1, 1, 1);

    m_pLabelPassword1 = new QLabel(m_pWidgetEncryption);
    m_pLabelPassword1->setObjectName(QStringLiteral("m_pLabelPassword1"));
    m_pLabelPassword1->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

    m_pLayoutEncryptionSettings->addWidget(m_pLabelPassword1, 1, 0, 1, 1);

    m_pEditorEncryptionPassword = new QLineEdit(m_pWidgetEncryption);
    m_pEditorEncryptionPassword->setObjectName(QStringLiteral("m_pEditorEncryptionPassword"));

    m_pLayoutEncryptionSettings->addWidget(m_pEditorEncryptionPassword, 1, 1, 1, 1);

    m_pLabelPassword2 = new QLabel(m_pWidgetEncryption);
    m_pLabelPassword2->setObjectName(QStringLiteral("m_pLabelPassword2"));
    m_pLabelPassword2->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

    m_pLayoutEncryptionSettings->addWidget(m_pLabelPassword2, 2, 0, 1, 1);

    m_pEditorEncryptionPasswordConfirm = new QLineEdit(m_pWidgetEncryption);
    m_pEditorEncryptionPasswordConfirm->setObjectName(QStringLiteral("m_pEditorEncryptionPasswordConfirm"));

    m_pLayoutEncryptionSettings->addWidget(m_pEditorEncryptionPasswordConfirm, 2, 1, 1, 1);


    m_pLayoutEncryption->addWidget(m_pWidgetEncryption, 1, 1, 1, 1);

    spacerItem1 = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);

    m_pLayoutEncryption->addItem(spacerItem1, 2, 1, 1, 1);

    m_pTabWidgetGeneral->addTab(m_pTabEncryption, QString());

    mLtMain->addWidget(m_pTabWidgetGeneral);

    m_pLabelSnapshot->setBuddy(mPsSnapshot);
    m_pLabelClipboard->setBuddy(mCbClipboard);
    m_pLabelDragAndDrop->setBuddy(mCbDragAndDrop);
    m_pLabelCipher->setBuddy(m_pComboCipher);
    m_pLabelPassword1->setBuddy(m_pEditorEncryptionPassword);
    m_pLabelPassword2->setBuddy(m_pEditorEncryptionPasswordConfirm);


    QObject::connect(m_pCheckBoxEncryption, SIGNAL(toggled(bool)), m_pWidgetEncryption, SLOT(setEnabled(bool)));

    m_pTabWidgetGeneral->setCurrentIndex(0);
}

void UIMachineSettingsGeneral::prepareTabBasic()
{
    /* Tab and it's layout created in the .ui file. */
    {
        /* Name and OS Type widget created in the .ui file. */
        AssertPtrReturnVoid(m_pNameAndSystemEditor);
        {
            /* Configure widget: */
            m_pNameAndSystemEditor->setNameFieldValidator(".+");
        }
    }
}

void UIMachineSettingsGeneral::prepareTabDescription()
{
    /* Tab and it's layout created in the .ui file. */
    {
        /* Description Text editor created in the .ui file. */
        AssertPtrReturnVoid(mTeDescription);
        {
            /* Configure editor: */
#ifdef VBOX_WS_MAC
            mTeDescription->setMinimumHeight(150);
#endif
        }
    }
}

void UIMachineSettingsGeneral::prepareTabEncryption()
{
    /* Tab and it's layout created in the .ui file. */
    {
        /* Encryption Cipher combo-box created in the .ui file. */
        AssertPtrReturnVoid(m_pComboCipher);
        {
            /* Configure combo-box: */
            m_encryptionCiphers << QString()
                                << "AES-XTS256-PLAIN64"
                                << "AES-XTS128-PLAIN64";
            m_pComboCipher->addItems(m_encryptionCiphers);
        }

        /* Encryption Password editor created in the .ui file. */
        AssertPtrReturnVoid(m_pEditorEncryptionPassword);
        {
            /* Configure editor: */
            m_pEditorEncryptionPassword->setEchoMode(QLineEdit::Password);
        }

        /* Encryption Password Confirmation editor created in the .ui file. */
        AssertPtrReturnVoid(m_pEditorEncryptionPasswordConfirm);
        {
            /* Configure editor: */
            m_pEditorEncryptionPasswordConfirm->setEchoMode(QLineEdit::Password);
        }
    }
}

void UIMachineSettingsGeneral::prepareConnections()
{
    /* Configure 'Basic' connections: */
    connect(m_pNameAndSystemEditor, &UINameAndSystemEditor::sigOsTypeChanged,
            this, &UIMachineSettingsGeneral::revalidate);
    connect(m_pNameAndSystemEditor, &UINameAndSystemEditor::sigNameChanged,
            this, &UIMachineSettingsGeneral::revalidate);

    /* Configure 'Encryption' connections: */
    connect(m_pCheckBoxEncryption, &QCheckBox::toggled,
            this, &UIMachineSettingsGeneral::revalidate);
    connect(m_pComboCipher, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &UIMachineSettingsGeneral::sltMarkEncryptionCipherChanged);
    connect(m_pComboCipher, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &UIMachineSettingsGeneral::revalidate);
    connect(m_pEditorEncryptionPassword, &QLineEdit::textEdited,
            this, &UIMachineSettingsGeneral::sltMarkEncryptionPasswordChanged);
    connect(m_pEditorEncryptionPassword, &QLineEdit::textEdited,
            this, &UIMachineSettingsGeneral::revalidate);
    connect(m_pEditorEncryptionPasswordConfirm, &QLineEdit::textEdited,
            this, &UIMachineSettingsGeneral::sltMarkEncryptionPasswordChanged);
    connect(m_pEditorEncryptionPasswordConfirm, &QLineEdit::textEdited,
            this, &UIMachineSettingsGeneral::revalidate);
}

void UIMachineSettingsGeneral::cleanup()
{
    /* Cleanup cache: */
    delete m_pCache;
    m_pCache = 0;
}

void UIMachineSettingsGeneral::repopulateComboClipboardMode()
{
    /* Clipboard mode combo-box created in the .ui file. */
    AssertPtrReturnVoid(mCbClipboard);
    {
        /* Clear combo first of all: */
        mCbClipboard->clear();

        /* Load currently supported Clipboard modes: */
        CSystemProperties comProperties = uiCommon().virtualBox().GetSystemProperties();
        QVector<KClipboardMode> clipboardModes = comProperties.GetSupportedClipboardModes();
        /* Take into account currently cached value: */
        const KClipboardMode enmCachedValue = m_pCache->base().m_clipboardMode;
        if (!clipboardModes.contains(enmCachedValue))
            clipboardModes.prepend(enmCachedValue);

        /* Populate combo finally: */
        foreach (const KClipboardMode &enmMode, clipboardModes)
            mCbClipboard->addItem(gpConverter->toString(enmMode), QVariant::fromValue(enmMode));
    }
}

void UIMachineSettingsGeneral::repopulateComboDnDMode()
{
    /* DnD mode combo-box created in the .ui file. */
    AssertPtrReturnVoid(mCbDragAndDrop);
    {
        /* Clear combo first of all: */
        mCbDragAndDrop->clear();

        /* Load currently supported DnD modes: */
        CSystemProperties comProperties = uiCommon().virtualBox().GetSystemProperties();
        QVector<KDnDMode> dndModes = comProperties.GetSupportedDnDModes();
        /* Take into account currently cached value: */
        const KDnDMode enmCachedValue = m_pCache->base().m_dndMode;
        if (!dndModes.contains(enmCachedValue))
            dndModes.prepend(enmCachedValue);

        /* Populate combo finally: */
        foreach (const KDnDMode &enmMode, dndModes)
            mCbDragAndDrop->addItem(gpConverter->toString(enmMode), QVariant::fromValue(enmMode));
    }
}

bool UIMachineSettingsGeneral::saveGeneralData()
{
    /* Prepare result: */
    bool fSuccess = true;
    /* Save general settings from the cache: */
    if (fSuccess && isMachineInValidMode() && m_pCache->wasChanged())
    {
        /* Save 'Basic' data from the cache: */
        if (fSuccess)
            fSuccess = saveBasicData();
        /* Save 'Advanced' data from the cache: */
        if (fSuccess)
            fSuccess = saveAdvancedData();
        /* Save 'Description' data from the cache: */
        if (fSuccess)
            fSuccess = saveDescriptionData();
        /* Save 'Encryption' data from the cache: */
        if (fSuccess)
            fSuccess = saveEncryptionData();
    }
    /* Return result: */
    return fSuccess;
}

bool UIMachineSettingsGeneral::saveBasicData()
{
    /* Prepare result: */
    bool fSuccess = true;
    /* Save 'Basic' data from the cache: */
    if (fSuccess)
    {
        /* Get old general data from the cache: */
        const UIDataSettingsMachineGeneral &oldGeneralData = m_pCache->base();
        /* Get new general data from the cache: */
        const UIDataSettingsMachineGeneral &newGeneralData = m_pCache->data();

        /* Save machine OS type ID: */
        if (isMachineOffline() && newGeneralData.m_strGuestOsTypeId != oldGeneralData.m_strGuestOsTypeId)
        {
            if (fSuccess)
            {
                m_machine.SetOSTypeId(newGeneralData.m_strGuestOsTypeId);
                fSuccess = m_machine.isOk();
            }
            if (fSuccess)
            {
                // Must update long mode CPU feature bit when os type changed:
                CVirtualBox vbox = uiCommon().virtualBox();
                // Should we check global object getters?
                const CGuestOSType &comNewType = vbox.GetGuestOSType(newGeneralData.m_strGuestOsTypeId);
                m_machine.SetCPUProperty(KCPUPropertyType_LongMode, comNewType.GetIs64Bit());
                fSuccess = m_machine.isOk();
            }
        }

        /* Show error message if necessary: */
        if (!fSuccess)
            notifyOperationProgressError(UIErrorString::formatErrorInfo(m_machine));
    }
    /* Return result: */
    return fSuccess;
}

bool UIMachineSettingsGeneral::saveAdvancedData()
{
    /* Prepare result: */
    bool fSuccess = true;
    /* Save 'Advanced' data from the cache: */
    if (fSuccess)
    {
        /* Get old general data from the cache: */
        const UIDataSettingsMachineGeneral &oldGeneralData = m_pCache->base();
        /* Get new general data from the cache: */
        const UIDataSettingsMachineGeneral &newGeneralData = m_pCache->data();

        /* Save machine clipboard mode: */
        if (fSuccess && newGeneralData.m_clipboardMode != oldGeneralData.m_clipboardMode)
        {
            m_machine.SetClipboardMode(newGeneralData.m_clipboardMode);
            fSuccess = m_machine.isOk();
        }
        /* Save machine D&D mode: */
        if (fSuccess && newGeneralData.m_dndMode != oldGeneralData.m_dndMode)
        {
            m_machine.SetDnDMode(newGeneralData.m_dndMode);
            fSuccess = m_machine.isOk();
        }
        /* Save machine snapshot folder: */
        if (fSuccess && isMachineOffline() && newGeneralData.m_strSnapshotsFolder != oldGeneralData.m_strSnapshotsFolder)
        {
            m_machine.SetSnapshotFolder(newGeneralData.m_strSnapshotsFolder);
            fSuccess = m_machine.isOk();
        }
        // VM name from 'Basic' data should go after the snapshot folder from the 'Advanced' data
        // as otherwise VM rename magic can collide with the snapshot folder one.
        /* Save machine name: */
        if (fSuccess && (isMachineOffline() || isMachineSaved()) && newGeneralData.m_strName != oldGeneralData.m_strName)
        {
            m_machine.SetName(newGeneralData.m_strName);
            fSuccess = m_machine.isOk();
        }

        /* Show error message if necessary: */
        if (!fSuccess)
            notifyOperationProgressError(UIErrorString::formatErrorInfo(m_machine));
    }
    /* Return result: */
    return fSuccess;
}

bool UIMachineSettingsGeneral::saveDescriptionData()
{
    /* Prepare result: */
    bool fSuccess = true;
    /* Save 'Description' data from the cache: */
    if (fSuccess)
    {
        /* Get old general data from the cache: */
        const UIDataSettingsMachineGeneral &oldGeneralData = m_pCache->base();
        /* Get new general data from the cache: */
        const UIDataSettingsMachineGeneral &newGeneralData = m_pCache->data();

        /* Save machine description: */
        if (fSuccess && newGeneralData.m_strDescription != oldGeneralData.m_strDescription)
        {
            m_machine.SetDescription(newGeneralData.m_strDescription);
            fSuccess = m_machine.isOk();
        }

        /* Show error message if necessary: */
        if (!fSuccess)
            notifyOperationProgressError(UIErrorString::formatErrorInfo(m_machine));
    }
    /* Return result: */
    return fSuccess;
}

bool UIMachineSettingsGeneral::saveEncryptionData()
{
    /* Prepare result: */
    bool fSuccess = true;
    /* Save 'Encryption' data from the cache: */
    if (fSuccess)
    {
        /* Get old general data from the cache: */
        const UIDataSettingsMachineGeneral &oldGeneralData = m_pCache->base();
        /* Get new general data from the cache: */
        const UIDataSettingsMachineGeneral &newGeneralData = m_pCache->data();

        /* Make sure it either encryption state is changed itself,
         * or the encryption was already enabled and either cipher or password is changed. */
        if (   isMachineOffline()
            && (   newGeneralData.m_fEncryptionEnabled != oldGeneralData.m_fEncryptionEnabled
                || (   oldGeneralData.m_fEncryptionEnabled
                    && (   newGeneralData.m_fEncryptionCipherChanged != oldGeneralData.m_fEncryptionCipherChanged
                        || newGeneralData.m_fEncryptionPasswordChanged != oldGeneralData.m_fEncryptionPasswordChanged))))
        {
            /* Get machine name for further activities: */
            QString strMachineName;
            if (fSuccess)
            {
                strMachineName = m_machine.GetName();
                fSuccess = m_machine.isOk();
            }
            /* Get machine attachments for further activities: */
            CMediumAttachmentVector attachments;
            if (fSuccess)
            {
                attachments = m_machine.GetMediumAttachments();
                fSuccess = m_machine.isOk();
            }

            /* Show error message if necessary: */
            if (!fSuccess)
                notifyOperationProgressError(UIErrorString::formatErrorInfo(m_machine));

            /* For each attachment: */
            for (int iIndex = 0; fSuccess && iIndex < attachments.size(); ++iIndex)
            {
                /* Get current attachment: */
                const CMediumAttachment &comAttachment = attachments.at(iIndex);

                /* Get attachment type for further activities: */
                KDeviceType enmType = KDeviceType_Null;
                if (fSuccess)
                {
                    enmType = comAttachment.GetType();
                    fSuccess = comAttachment.isOk();
                }
                /* Get attachment medium for further activities: */
                CMedium comMedium;
                if (fSuccess)
                {
                    comMedium = comAttachment.GetMedium();
                    fSuccess = comAttachment.isOk();
                }

                /* Show error message if necessary: */
                if (!fSuccess)
                    notifyOperationProgressError(UIErrorString::formatErrorInfo(comAttachment));
                else
                {
                    /* Pass hard-drives only: */
                    if (enmType != KDeviceType_HardDisk)
                        continue;

                    /* Get medium id for further activities: */
                    QUuid uMediumId;
                    if (fSuccess)
                    {
                        uMediumId = comMedium.GetId();
                        fSuccess = comMedium.isOk();
                    }

                    /* Create encryption update progress: */
                    CProgress comProgress;
                    if (fSuccess)
                    {
                        /* Cipher attribute changed? */
                        QString strNewCipher;
                        if (newGeneralData.m_fEncryptionCipherChanged)
                        {
                            strNewCipher = newGeneralData.m_fEncryptionEnabled ?
                                           m_encryptionCiphers.at(newGeneralData.m_iEncryptionCipherIndex) : QString();
                        }

                        /* Password attribute changed? */
                        QString strNewPassword;
                        QString strNewPasswordId;
                        if (newGeneralData.m_fEncryptionPasswordChanged)
                        {
                            strNewPassword = newGeneralData.m_fEncryptionEnabled ?
                                             newGeneralData.m_strEncryptionPassword : QString();
                            strNewPasswordId = newGeneralData.m_fEncryptionEnabled ?
                                               strMachineName : QString();
                        }

                        /* Get the maps of encrypted media and their passwords: */
                        const EncryptedMediumMap &encryptedMedium = newGeneralData.m_encryptedMedia;
                        const EncryptionPasswordMap &encryptionPasswords = newGeneralData.m_encryptionPasswords;

                        /* Check if old password exists/provided: */
                        const QString strOldPasswordId = encryptedMedium.key(uMediumId);
                        const QString strOldPassword = encryptionPasswords.value(strOldPasswordId);

                        /* Create encryption progress: */
                        comProgress = comMedium.ChangeEncryption(strOldPassword,
                                                                 strNewCipher,
                                                                 strNewPassword,
                                                                 strNewPasswordId);
                        fSuccess = comMedium.isOk();
                    }

                    /* Create encryption update progress dialog: */
                    QPointer<UIProgress> pDlg;
                    if (fSuccess)
                    {
                        pDlg = new UIProgress(comProgress);
                        connect(pDlg.data(), &UIProgress::sigProgressChange,
                                this, &UIMachineSettingsGeneral::sigOperationProgressChange,
                                Qt::QueuedConnection);
                        connect(pDlg.data(), &UIProgress::sigProgressError,
                            this, &UIMachineSettingsGeneral::sigOperationProgressError,
                                Qt::BlockingQueuedConnection);
                        pDlg->run(350);
                        if (pDlg)
                            delete pDlg;
                        else
                        {
                            // Premature application shutdown,
                            // exit immediately:
                            return true;
                        }
                    }

                    /* Show error message if necessary: */
                    if (!fSuccess)
                        notifyOperationProgressError(UIErrorString::formatErrorInfo(comMedium));
                }
            }
        }
    }
    /* Return result: */
    return fSuccess;
}
