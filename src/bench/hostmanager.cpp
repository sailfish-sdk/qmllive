/****************************************************************************
**
** Copyright (C) 2016 Pelagicore AG
** Copyright (C) 2016 Jolla Ltd
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QmlLive tool.
**
** $QT_BEGIN_LICENSE:GPL-QTAS$
** Commercial License Usage
** Licensees holding valid commercial Qt Automotive Suite licenses may use
** this file in accordance with the commercial license agreement provided
** with the Software or, alternatively, in accordance with the terms
** contained in a written agreement between you and The Qt Company.  For
** licensing terms and conditions see https://www.qt.io/terms-conditions.
** For further information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
** SPDX-License-Identifier: GPL-3.0
**
****************************************************************************/

#include "hostmanager.h"
#include "hostmodel.h"
#include "hostwidget.h"
#include "dummydelegate.h"
#include "livehubengine.h"
#include "logreceiver.h"
#include "widgets/logview.h"

#include <QDockWidget>
#include <QDebug>
#include <QFileInfo>

HostManager::HostManager(QWidget *parent) :
    QListView(parent)
{
    setFrameStyle(QFrame::StyledPanel);
    setAlternatingRowColors(false);
    setItemDelegate(new DummyDelegate(this));
    setFlow(QListView::LeftToRight);
    setUniformItemSizes(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setDragEnabled(true);
    setDefaultDropAction(Qt::MoveAction);
    setDragDropMode(QAbstractItemView::InternalMove);
    viewport()->setAcceptDrops(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);

    viewport()->setAutoFillBackground(false);
}

void HostManager::setModel(HostModel *model)
{
    m_model = model;
    QListView::setModel(model);

    connect(model, &HostModel::modelReset, this, &HostManager::modelReseted);
}

void HostManager::setLiveHubEngine(LiveHubEngine *engine)
{
    m_engine = engine;

    for (int i=0; i < m_model->rowCount(); i++) {
        HostWidget *widget = qobject_cast<HostWidget*>(indexWidget(m_model->index(i, 0)));
        if (widget)
            widget->setLiveHubEngine(engine);
    }
}

void HostManager::followTreeSelection(const LiveDocument &currentFile)
{
    if (!currentFile.isFileIn(m_engine->workspace()))
        return;

    for (int i=0; i < m_model->rowCount(); i++) {
        HostWidget *widget = qobject_cast<HostWidget*>(indexWidget(m_model->index(i, 0)));
        if (widget && widget->followTreeSelection())
            widget->setCurrentFile(currentFile);
    }
}

void HostManager::setCurrentFile(const LiveDocument &currentFile)
{
    if (!currentFile.isFileIn(m_engine->workspace()))
        return;

    for (int i=0; i < m_model->rowCount(); i++) {
        HostWidget *widget = qobject_cast<HostWidget*>(indexWidget(m_model->index(i, 0)));
        if (widget)
            widget->setCurrentFile(currentFile);
    }
}

void HostManager::publishAll()
{
    for (int i=0; i < m_model->rowCount(); i++) {
        HostWidget *widget = qobject_cast<HostWidget*>(indexWidget(m_model->index(i, 0)));
        if (widget)
            widget->publishWorkspace();
    }
}

void HostManager::refreshAll()
{
    for (int i=0; i < m_model->rowCount(); i++) {
        HostWidget *widget = qobject_cast<HostWidget*>(indexWidget(m_model->index(i, 0)));
        if (widget)
            widget->refresh();
    }
}

void HostManager::probe(const QString &hostName)
{
    int index = m_model->indexOf(hostName);
    if (index < 0) {
        qWarning() << "No such host: " << hostName;
        return;
    }

    HostWidget *widget = qobject_cast<HostWidget*>(indexWidget(m_model->index(index, 0)));
    Q_ASSERT(widget);
    widget->probe();
}

void HostManager::rowsInserted(const QModelIndex &parent, int start, int end)
{
    if (parent.isValid())
        return;

    for (int i=start; i<= end; i++) {
        addHost(i);
    }
}

void HostManager::addHost(int index)
{
    HostWidget *widget = new HostWidget();
    Host *host = m_model->hostAt(index);
    widget->setLiveHubEngine(m_engine.data());
    widget->setHost(host);
    setIndexWidget(m_model->index(index,0), widget);
    connect(widget, &HostWidget::openHostConfig, this, &HostManager::openHostConfig);

    QDockWidget *dock = new QDockWidget(host->name());
    dock->setObjectName(host->name() + "LogDock");
    connect(host, &Host::nameChanged, dock, &QDockWidget::setWindowTitle);
    LogView *view = new LogView(false, dock);
    connect(widget, &HostWidget::remoteLog, view, &LogView::appendToLog);
    connect(widget, &HostWidget::clearLog, view, &LogView::clear);
    connect(widget, &HostWidget::connected, view, &LogView::clear);
    dock->setWidget(view);
    m_logList.append(dock);
    emit logWidgetAdded(dock);
}

void HostManager::rowsAboutToBeRemoved(const QModelIndex & parent, int start, int end)
{
    if (parent.isValid())
        return;

    for (int i=start; i<= end; i++) {
        QDockWidget *dock = m_logList.takeAt(i);
        delete dock;
    }
}

void HostManager::modelReseted()
{
    for (int i=0; i < m_model->rowCount(); i++) {
        addHost(i);
    }
}
