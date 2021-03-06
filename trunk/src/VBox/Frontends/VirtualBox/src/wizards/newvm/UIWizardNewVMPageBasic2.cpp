/* $Id$ */
/** @file
 * VBox Qt GUI - UIWizardNewVMPageBasic2 class implementation.
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
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QToolBox>
#include <QVBoxLayout>

/* GUI includes: */
#include "QIRichTextLabel.h"
#include "UIFilePathSelector.h"
#include "UIIconPool.h"
#include "UIUserNamePasswordEditor.h"
#include "UIWizardNewVMPageBasic2.h"
#include "UIWizardNewVM.h"


UIWizardNewVMPage2::UIWizardNewVMPage2()
    : m_pUserNamePasswordEditor(0)
    , m_pHostnameLineEdit(0)
    , m_pHostnameLabel(0)
    , m_pGAISOPathLabel(0)
    , m_pGAISOFilePathSelector(0)
    , m_pProductKeyLineEdit(0)
    , m_pProductKeyLabel(0)

{
}

QString UIWizardNewVMPage2::userName() const
{
    if (m_pUserNamePasswordEditor)
        return m_pUserNamePasswordEditor->userName();
    return QString();
}

void UIWizardNewVMPage2::setUserName(const QString &strName)
{
    if (m_pUserNamePasswordEditor)
        return m_pUserNamePasswordEditor->setUserName(strName);
}

QString UIWizardNewVMPage2::password() const
{
    if (m_pUserNamePasswordEditor)
        return m_pUserNamePasswordEditor->password();
    return QString();
}

void UIWizardNewVMPage2::setPassword(const QString &strPassword)
{
    if (m_pUserNamePasswordEditor)
        return m_pUserNamePasswordEditor->setPassword(strPassword);
}

QString UIWizardNewVMPage2::hostname() const
{
    if (m_pHostnameLineEdit)
        return m_pHostnameLineEdit->text();
    return QString();
}

void UIWizardNewVMPage2::setHostname(const QString &strHostName)
{
    if (m_pHostnameLineEdit)
        return m_pHostnameLineEdit->setText(strHostName);
}

bool UIWizardNewVMPage2::installGuestAdditions() const
{
    if (!m_pGAISOFilePathSelector)
        return false;
    return m_pGAISOFilePathSelector->isValid();
}

QString UIWizardNewVMPage2::guestAdditionsISOPath() const
{
    if (!m_pGAISOFilePathSelector)
        return QString();
    return m_pGAISOFilePathSelector->path();
}

void UIWizardNewVMPage2::setGuestAdditionsISOPath(const QString &strISOPath)
{
    if (m_pGAISOFilePathSelector)
        m_pGAISOFilePathSelector->setPath(strISOPath);
}

QString UIWizardNewVMPage2::productKey() const
{
    if (!m_pProductKeyLineEdit || !m_pProductKeyLineEdit->hasAcceptableInput())
        return QString();
    return m_pProductKeyLineEdit->text();
}

QWidget *UIWizardNewVMPage2::createUserNameHostNameWidgets()
{
    QWidget *pContainer = new QWidget;
    QGridLayout *pGridLayout = new QGridLayout(pContainer);

    m_pUserNamePasswordEditor = new UIUserNamePasswordEditor;
    pGridLayout->addWidget(m_pUserNamePasswordEditor, 0, 0, 1, 4);

    m_pHostnameLabel = new QLabel;
    m_pHostnameLabel->setAlignment(Qt::AlignRight);
    m_pHostnameLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    m_pHostnameLineEdit = new QLineEdit;

    pGridLayout->addWidget(m_pHostnameLabel,    1, 0, 1, 1);
    pGridLayout->addWidget(m_pHostnameLineEdit, 1, 1, 1, 3);

    return pContainer;
}

QWidget *UIWizardNewVMPage2::createGAInstallWidgets()
{
    QWidget *pContainer = new QWidget;
    QGridLayout *pContainerLayout = new QGridLayout(pContainer);

    m_pGAISOPathLabel = new QLabel;
    {
        m_pGAISOPathLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
        m_pGAISOPathLabel->setEnabled(false);
    }
    m_pGAISOFilePathSelector = new UIFilePathSelector;
    {
        m_pGAISOFilePathSelector->setResetEnabled(false);
        m_pGAISOFilePathSelector->setMode(UIFilePathSelector::Mode_File_Open);
        m_pGAISOFilePathSelector->setFileDialogFilters("*.iso *.ISO");
        m_pGAISOFilePathSelector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        m_pGAISOFilePathSelector->setEnabled(false);
    }

    pContainerLayout->addWidget(m_pGAISOPathLabel, 1, 1, 1, 1);
    pContainerLayout->addWidget(m_pGAISOFilePathSelector, 1, 2, 1, 4);
    return pContainer;
}

QWidget *UIWizardNewVMPage2::createProductKeyWidgets()
{
    QWidget *pContainer = new QWidget;
    QGridLayout *pGridLayout = new QGridLayout(pContainer);

    m_pProductKeyLabel = new QLabel;
    m_pProductKeyLabel->setAlignment(Qt::AlignRight);

    m_pProductKeyLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    m_pProductKeyLineEdit = new QLineEdit;
    m_pProductKeyLineEdit->setInputMask(">NNNNN-NNNNN-NNNNN-NNNNN-NNNNN;#");
    pGridLayout->addWidget(m_pProductKeyLabel, 0, 0, 1, 1);
    pGridLayout->addWidget(m_pProductKeyLineEdit, 0, 1, 1, 3);
    return pContainer;
}

bool UIWizardNewVMPage2::checkGAISOFile() const
{
    const QString &strPath = m_pGAISOFilePathSelector->path();
    if (strPath.isNull() || strPath.isEmpty())
        return true;
    QFile fileInfo(strPath);
    if (!fileInfo.exists() || !fileInfo.isReadable())
        return false;
    return true;
}

void UIWizardNewVMPage2::markWidgets() const
{
    if (m_pGAISOFilePathSelector)
        m_pGAISOFilePathSelector->mark(!checkGAISOFile());
}

void UIWizardNewVMPage2::retranslateWidgets()
{
    if (m_pHostnameLabel)
        m_pHostnameLabel->setText(UIWizardNewVM::tr("Hostname:"));

    if (m_pGAISOPathLabel)
        m_pGAISOPathLabel->setText(UIWizardNewVM::tr("GA Installation ISO:"));
    if (m_pGAISOFilePathSelector)
        m_pGAISOFilePathSelector->setToolTip(UIWizardNewVM::tr("Please select an installation medium (ISO file)"));
    if (m_pProductKeyLabel)
        m_pProductKeyLabel->setText(UIWizardNewVM::tr("Product Key:"));
}

UIWizardNewVMPageBasic2::UIWizardNewVMPageBasic2()
    : m_pLabel(0)
    , m_pToolBox(0)
{
    prepare();
}

void UIWizardNewVMPageBasic2::prepare()
{
    QVBoxLayout *pMainLayout = new QVBoxLayout(this);
    m_pToolBox = new QToolBox;
    pMainLayout->addWidget(m_pToolBox);

    {
        m_pLabel = new QIRichTextLabel(this);
        if (m_pLabel)
            pMainLayout->addWidget(m_pLabel);
        pMainLayout->addWidget(m_pToolBox);
        pMainLayout->addStretch();
    }

    m_pToolBox->insertItem(ToolBoxItems_UserNameHostname, createUserNameHostNameWidgets(), QString());
    m_pToolBox->insertItem(ToolBoxItems_GAInstall, createGAInstallWidgets(), QString());
    m_pToolBox->insertItem(ToolBoxItems_ProductKey, createProductKeyWidgets(), QString());

    registerField("userName", this, "userName");
    registerField("password", this, "password");
    registerField("hostname", this, "hostname");
    registerField("installGuestAdditions", this, "installGuestAdditions");
    registerField("guestAdditionsISOPath", this, "guestAdditionsISOPath");
    registerField("productKey", this, "productKey");

    createConnections();
}

void UIWizardNewVMPageBasic2::createConnections()
{
    if (m_pUserNamePasswordEditor)
        connect(m_pUserNamePasswordEditor, &UIUserNamePasswordEditor::sigSomeTextChanged,
                this, &UIWizardNewVMPageBasic2::completeChanged);
    if (m_pGAISOFilePathSelector)
        connect(m_pGAISOFilePathSelector, &UIFilePathSelector::pathChanged,
                this, &UIWizardNewVMPageBasic2::sltGAISOPathChanged);
}

void UIWizardNewVMPageBasic2::retranslateUi()
{
    setTitle(UIWizardNewVM::tr("Unattended Guest OS Install Setup"));
    if (m_pLabel)
        m_pLabel->setText(UIWizardNewVM::tr("<p>Here you can configure the unattended install by modifying username, password, and "
                                            "hostname. You can additionally enable guest additions install. "
                                            "For Microsoft Windows guests it is possible to provide a product key..</p>"));
    retranslateWidgets();
    if (m_pToolBox)
    {
        m_pToolBox->setItemText(ToolBoxItems_UserNameHostname, UIWizardNewVM::tr("Username and hostname"));
        m_pToolBox->setItemText(ToolBoxItems_GAInstall, UIWizardNewVM::tr("Guest additions install"));
        m_pToolBox->setItemText(ToolBoxItems_ProductKey, UIWizardNewVM::tr("Product key"));
    }
}

void UIWizardNewVMPageBasic2::initializePage()
{
    retranslateUi();
}

bool UIWizardNewVMPageBasic2::isComplete() const
{
    AssertReturn(m_pToolBox, false);

    m_pToolBox->setItemIcon(ToolBoxItems_UserNameHostname, QIcon());
    m_pToolBox->setItemIcon(ToolBoxItems_GAInstall, QIcon());
    m_pToolBox->setItemIcon(ToolBoxItems_ProductKey, QIcon());

    markWidgets();
    bool fIsComplete = true;
    if (!checkGAISOFile())
    {
        m_pToolBox->setItemIcon(ToolBoxItems_GAInstall, UIIconPool::iconSet(":/status_error_16px.png"));
        fIsComplete = false;
    }
    if (m_pUserNamePasswordEditor && !m_pUserNamePasswordEditor->isComplete())
    {
        m_pToolBox->setItemIcon(ToolBoxItems_UserNameHostname, UIIconPool::iconSet(":/status_error_16px.png"));
        fIsComplete = false;
    }
    return fIsComplete;
}

void UIWizardNewVMPageBasic2::cleanupPage()
{
}

void UIWizardNewVMPageBasic2::showEvent(QShowEvent *pEvent)
{
    if (m_pToolBox)
        m_pToolBox->setItemEnabled(ToolBoxItems_ProductKey, isProductKeyWidgetEnabled());
    UIWizardPage::showEvent(pEvent);
}

void UIWizardNewVMPageBasic2::sltInstallGACheckBoxToggle(bool fEnabled)
{
    if (m_pGAISOPathLabel)
        m_pGAISOPathLabel->setEnabled(fEnabled);
    if (m_pGAISOFilePathSelector)
        m_pGAISOFilePathSelector->setEnabled(fEnabled);
    emit completeChanged();
}

void UIWizardNewVMPageBasic2::sltGAISOPathChanged(const QString &strPath)
{
    Q_UNUSED(strPath);
    emit completeChanged();
}

bool UIWizardNewVMPageBasic2::isProductKeyWidgetEnabled() const
{
    UIWizardNewVM *pWizard = qobject_cast<UIWizardNewVM*>(wizard());
    if (!pWizard || !pWizard->isUnattendedEnabled() || !pWizard->isGuestOSTypeWindows())
        return false;
    return true;
}
