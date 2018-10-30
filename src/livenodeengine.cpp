/****************************************************************************
**
** Copyright (C) 2016 Pelagicore AG
** Copyright (C) 2016,2018 Jolla Ltd
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

#include "livenodeengine.h"
#include "liveruntime.h"
#include "qmlhelper.h"
#include "resourcemap.h"
#include "contentpluginfactory.h"
#include "imageadapter.h"
#include "fontadapter.h"

#include "QtQml/qqml.h"
#include "QtQuick/private/qquickpixmapcache_p.h"

// TODO: create proxy configuration settings, controlled by command line and ui

#ifdef QMLLIVE_DEBUG
#define DEBUG qDebug()
#else
#define DEBUG if (0) qDebug()
#endif

namespace {
const char *const OVERLAY_PATH_PREFIX = "qml-live-overlay--";
const char OVERLAY_PATH_SEPARATOR = '-';
}

/*!
 * \class LiveNodeEngine
 * \brief The LiveNodeEngine class instantiates QML components in cooperation with LiveHubEngine.
 * \inmodule qmllive
 *
 * LiveNodeEngine provides ways to reload QML documents based incoming requests
 * from a hub. A hub can be connected via a RemotePublisher/RemoteReceiver pair.
 *
 * The primary use case is to allow loading of QML components instantiating
 * QQuickWindow, i.e., inheriting QML Window. A fallbackView can be set in order
 * to support also QML Item based components.
 *
 * In Addition to showing QML files the LiveNodeEngine can be extended by plugins to show any other filetype.
 * One need to set the Plugin path to the right destination and the LiveNodeEngine will load all the plugins
 * it finds there.
 *
 * \sa {Custom Runtime}, {ContentPlugin Example}
 */

/*!
 *   \enum LiveNodeEngine::WorkspaceOption
 *
 *   This enum type controls optional workspace related features:
 *
 *   \value NoWorkspaceOption
 *          No optional feature is enabled.
 *   \value LoadDummyData
 *          Enables loading of dummy QML data - QML documents located in the
 *          "dummydata" subdirectory of the workspace directory.
 *   \value AllowUpdates
 *          Enables receiving updates to workspace documents.
 *   \value UpdatesAsOverlay
 *          With this option enabled, updates can be received even if workspace
 *          is read only. Updates will be stored in a writable overlay stacked
 *          over the original workspace with the help of
 *          QQmlAbstractUrlInterceptor. This option only influences the way how
 *          updates to file system files are handled - updates to Qt resource
 *          files are always stored in an overlay.
 *          Requires \l AllowUpdates.
 *   \value AllowCreateMissing
 *          Without this option enabled, updates are only accepted for existing
 *          workspace documents. Requires \l AllowUpdates.
 *
 * \sa {QmlLive Runtime}
 */

// Overlay uses temporary directory to allow parallel execution
class Overlay : public QObject
{
    Q_OBJECT

public:
    Overlay(const QString &basePath, QObject *parent)
        : QObject(parent)
        , m_basePath(basePath)
        , m_overlay(overlayTemplatePath())
    {
        if (!m_overlay.isValid())
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
            qFatal("Failed to create overlay directory: %s", qPrintable(m_overlay.errorString()));
#else
            qFatal("Failed to create overlay directory");
#endif

    }

    ~Overlay()
    {
    }

    QString reserve(const LiveDocument &document, bool existing)
    {
        QWriteLocker locker(&m_lock);

        QString overlayingPath = document.absoluteFilePathIn(m_overlay.path());
        m_mappings.insert(document.absoluteFilePathIn(m_basePath), qMakePair(overlayingPath, existing));
        return overlayingPath;
    }

    QString map(const QString &file, bool existingOnly) const
    {
        QReadLocker locker(&m_lock);

        auto it = m_mappings.find(file);
        if (it == m_mappings.end())
            return file;
        if (existingOnly && !it->second)
            return file;
        return it->first;
    }

private:
    static QString overlayTemplatePath()
    {
        QSettings settings;
        QString overlayTemplatePath = QDir::tempPath() + QDir::separator() + QLatin1String(OVERLAY_PATH_PREFIX);
        if (!settings.organizationName().isEmpty()) // See QCoreApplication::organizationName's docs
            overlayTemplatePath += settings.organizationName() + QLatin1Char(OVERLAY_PATH_SEPARATOR);
        overlayTemplatePath += settings.applicationName();
        return overlayTemplatePath;
    }

private:
    mutable QReadWriteLock m_lock;
    // base path -> ( overlaying path, file exists at base path )
    QHash<QString, QPair<QString, bool>> m_mappings;
    QString m_basePath;
    QTemporaryDir m_overlay;
};

class UrlInterceptor : public QObject, public QQmlAbstractUrlInterceptor
{
    Q_OBJECT

public:
    UrlInterceptor(const QDir &workspace, const Overlay *overlay, const ResourceMap *resourceMap,
            QQmlAbstractUrlInterceptor *otherInterceptor, QObject *parent)
        : QObject(parent)
        , m_otherInterceptor(otherInterceptor)
        , m_workspace(workspace)
        , m_overlay(overlay)
        , m_resourceMap(resourceMap)
    {
        Q_ASSERT(overlay);
        Q_ASSERT(resourceMap);
    }

    // From QQmlAbstractUrlInterceptor
    QUrl intercept(const QUrl &url, DataType type) Q_DECL_OVERRIDE
    {
        const QUrl url_ = m_otherInterceptor ? m_otherInterceptor->intercept(url, type) : url;

        if (url_.scheme() == QLatin1String("file")) {
            bool existingOnly = true;
            return QUrl::fromLocalFile(m_overlay->map(url_.toLocalFile(), existingOnly));
        } else if (url_.scheme() == QLatin1String("qrc")) {
            const LiveDocument document = LiveDocument::resolve(m_workspace, *m_resourceMap, url_);
            if (document.isNull())
                return url_;

            QString filePath = document.absoluteFilePathIn(m_workspace);
            bool existingOnly = false;
            filePath = m_overlay->map(filePath, existingOnly);
            if (!QFileInfo(filePath).exists())
                return url_;

            return QUrl::fromLocalFile(filePath);
        } else {
            return url_;
        }
    }

private:
    QQmlAbstractUrlInterceptor *m_otherInterceptor;
    const QDir m_workspace;
    const QPointer<const Overlay> m_overlay;
    const QPointer<const ResourceMap> m_resourceMap;
};

/*!
 * Standard constructor using \a parent as parent
 */
LiveNodeEngine::LiveNodeEngine(QObject *parent)
    : QObject(parent)
    , m_runtime(new LiveRuntime(this))
    , m_xOffset(0)
    , m_yOffset(0)
    , m_rotation(0)
    , m_resourceMap(new ResourceMap(this))
    , m_delayReload(new QTimer(this))
    , m_pluginFactory(new ContentPluginFactory(this))
    , m_activePlugin(0)
{
    m_delayReload->setInterval(250);
    m_delayReload->setSingleShot(true);
    connect(m_delayReload, &QTimer::timeout, this, &LiveNodeEngine::reloadDocument);
}

/*!
 * Destructor
 */
LiveNodeEngine::~LiveNodeEngine()
{
}

/*!
 * The QML engine to be used for loading QML components
 */
QQmlEngine *LiveNodeEngine::qmlEngine() const
{
    return m_qmlEngine;
}

/*!
 * Sets \a qmlEngine as the QML engine to be used for loading QML components
 */
void LiveNodeEngine::setQmlEngine(QQmlEngine *qmlEngine)
{
    Q_ASSERT(!this->qmlEngine());
    Q_ASSERT(qmlEngine);

    m_qmlEngine = qmlEngine;

    connect(m_qmlEngine.data(), &QQmlEngine::warnings, this, &LiveNodeEngine::logErrors);

    m_qmlEngine->rootContext()->setContextProperty("livert", m_runtime);
}

/*!
 * The QQuickView for displaying QML Item based components, i.e., those not
 * creating own windows.
 */
QQuickView *LiveNodeEngine::fallbackView() const
{
    return m_fallbackView;
}

/*!
 * Sets \a fallbackView as the QQuickView for displaying QML Item based
 * components, i.e., those not creating own windows.
 */
void LiveNodeEngine::setFallbackView(QQuickView *fallbackView)
{
    Q_ASSERT(qmlEngine());
    Q_ASSERT(fallbackView);
    Q_ASSERT(fallbackView->engine() == qmlEngine());

    if (fallbackView && fallbackView->engine() != m_qmlEngine)
        qCritical() << "LiveNodeEngine::fallbackView must use the QmlEngine instance set as LiveNodeEngine::qmlEngine";

    m_fallbackView = fallbackView;
}

/*!
 * Sets the x-offset \a offset of window
 */
void LiveNodeEngine::setXOffset(int offset)
{
    if (m_activeWindow)
        m_activeWindow->contentItem()->setX(offset);

    m_xOffset = offset;
}

/*!
 * Returns the current x-offset of the window
 */
int LiveNodeEngine::xOffset() const
{
    return m_xOffset;
}

/*!
 * Sets the y-offset \a offset of window
 */
void LiveNodeEngine::setYOffset(int offset)
{
    if (m_activeWindow)
        m_activeWindow->contentItem()->setY(offset);

    m_yOffset = offset;
}

/*!
 * Returns the current y-offset of the window
 */
int LiveNodeEngine::yOffset() const
{
    return m_yOffset;
}

/*!
 * Sets the rotation \a rotation of window around the center
 */
void LiveNodeEngine::setRotation(int rotation)
{
    if (m_activeWindow) {
        m_activeWindow->contentItem()->setRotation(0);
        const QPointF center(m_activeWindow->width() / 2, m_activeWindow->height() / 2);
        m_activeWindow->contentItem()->setTransformOriginPoint(center);
        m_activeWindow->contentItem()->setRotation(rotation);
    }

    m_rotation = rotation;
}

/*!
 * Return the current rotation angle
 */
int LiveNodeEngine::rotation() const
{
    return m_rotation;
}

/*!
 * Allows to initialize active document with an instance preloaded beyond
 * LiveNodeEngine's control.
 *
 * This can be called at most once and only before a document has been loaded
 * with loadDocument().
 *
 * \a document is the source of the component that was used to instantiate the
 * \a object. \a window should be either the \a object itself or the
 * fallbackView(). \a errors (if any) will be added to log.
 *
 * Note that \a window will be destroyed on next loadDocument() call unless it
 * is the fallbackView(). \a object will be destroyed unconditionally.
 */
void LiveNodeEngine::usePreloadedDocument(const LiveDocument &document, QObject *object,
                                          QQuickWindow *window, const QList<QQmlError> &errors)
{
    LIVE_ASSERT(m_activeFile.isNull(), return);
    LIVE_ASSERT(!document.isNull(), return);

    m_activeFile = document;

    if (!m_activeFile.existsIn(m_workspace) && !m_activeFile.mapsToResource(*m_resourceMap)) {
        QQmlError error;
        error.setUrl(QUrl::fromLocalFile(m_activeFile.absoluteFilePathIn(m_workspace)));
        error.setDescription(tr("File not found under the workspace "
                    "and no mapping to a Qt resource exists for that file"));
        emit logErrors(QList<QQmlError>() << error);
    }

    m_object = object;
    m_activeWindow = window;

    if (m_activeWindow) {
        m_activeWindowConnections << connect(m_activeWindow.data(), &QWindow::widthChanged,
                                             this, &LiveNodeEngine::onSizeChanged);
        m_activeWindowConnections << connect(m_activeWindow.data(), &QWindow::heightChanged,
                                             this, &LiveNodeEngine::onSizeChanged);
        onSizeChanged();
    }

    emit activeDocumentChanged(m_activeFile);
    emit documentLoaded();
    emit activeWindowChanged(m_activeWindow);
    emit logErrors(errors);
}

/*!
 * This is an overloaded function provided for convenience. It is suitable for
 * use with QQmlApplicationEngine.
 *
 * Tries to resolve \a document against current workspace(). \a window is the
 * root object. \a errors (if any) will be added to log.
 */
void LiveNodeEngine::usePreloadedDocument(const QString &document, QQuickWindow *window,
                                          const QList<QQmlError> &errors)
{
    LIVE_ASSERT(m_activeFile.isNull(), return);

    LiveDocument resolved = LiveDocument::resolve(workspace(), *m_resourceMap, document);
    if (resolved.isNull()) {
        qWarning() << "Failed to resolve preloaded document path:" << document
                   << "Workspace: " << workspace();
        return;
    }

    usePreloadedDocument(resolved, window, window, errors);
}

/*!
 * Loads or reloads the given \a document onto the QML view. Clears any caches.
 *
 * The activeDocumentChanged() signal is emitted when this results in change of
 * the activeDocument().
 *
 * \sa documentLoaded()
 */
void LiveNodeEngine::loadDocument(const LiveDocument& document)
{
    DEBUG << "LiveNodeEngine::loadDocument: " << document;

    LiveDocument oldActiveFile = m_activeFile;

    m_activeFile = document;

    if (m_activeFile != oldActiveFile)
        emit activeDocumentChanged(m_activeFile);

    if (!m_activeFile.isNull())
        reloadDocument();
}

/*!
 * Starts a timer to reload the view with a delay.
 *
 * A delay reload is important to avoid constant reloads, while many changes
 * appear.
 */
void LiveNodeEngine::delayReload()
{
    m_delayReload->start();
}

/*!
 * Checks if the QtQuick Controls module exists for the content adapters
 */
void LiveNodeEngine::checkQmlFeatures()
{
    foreach (const QString &importPath, m_qmlEngine->importPathList()) {
        QDir dir(importPath);
        if (dir.exists("QtQuick/Controls") &&
            dir.exists("QtQuick/Layouts") &&
            dir.exists("QtQuick/Dialogs")) {
            m_quickFeatures |= ContentAdapterInterface::QtQuickControls;
        }
    }
}

QUrl LiveNodeEngine::errorScreenUrl() const
{
    return m_quickFeatures.testFlag(ContentAdapterInterface::QtQuickControls)
        ? QUrl("qrc:/livert/error_qt5_controls.qml")
        : QUrl("qrc:/livert/error_qt5.qml");
}

/*!
 * Reloads the active QML document.
 *
 * Emits documentLoaded() when finished.
 *
 * If \l fallbackView is set, its \c source will be cleared, whether the view
 * was previously used or not.
 */
void LiveNodeEngine::reloadDocument()
{
    Q_ASSERT(qmlEngine());

    while (!m_activeWindowConnections.isEmpty()) {
        disconnect(m_activeWindowConnections.takeLast());
    }

    // Do this unconditionally!
    if (m_fallbackView)
        m_fallbackView->setSource(QUrl());

    m_activeWindow = 0;

    delete m_object;

    QQuickPixmap::purgeCache();
    m_qmlEngine->clearComponentCache();

    checkQmlFeatures();

    qInfo() << "----------------------------------------";
    qInfo() << "QmlLive: (Re)loading" << m_activeFile;

    emit clearLog();

    const QUrl originalUrl = m_activeFile.runtimeLocation(m_workspace, *m_resourceMap);
    const QUrl url = queryDocumentViewer(originalUrl);

    DEBUG << "Loading document" << m_activeFile << "runtime location:" << originalUrl;
    if (url != originalUrl)
        DEBUG << "Using viewer" << url;

    auto showErrorScreen = [this] {
        Q_ASSERT(m_fallbackView);
        m_fallbackView->setResizeMode(QQuickView::SizeRootObjectToView);
        m_fallbackView->setSource(errorScreenUrl());
        m_activeWindow = m_fallbackView;
    };

    auto logError = [this, url](const QString &description) {
        QQmlError error;
        error.setObject(m_object);
        error.setUrl(url);
        error.setLine(0);
        error.setColumn(0);
        error.setDescription(description);
        emit logErrors(QList<QQmlError>() << error);
    };

    QScopedPointer<QQmlComponent> component(new QQmlComponent(m_qmlEngine));
    if (url.path().endsWith(QLatin1String(".qml"), Qt::CaseInsensitive)) {
        component->loadUrl(url);
        m_object = component->create();
    } else if (url == originalUrl) {
        logError(tr("LiveNodeEngine: Cannot display this file type"));
    } else {
        logError(tr("LiveNodeEngine: Internal error: Cannot display this file type"));
    }

    if (!component->isReady()) {
        if (component->isLoading()) {
            qCritical() << "Component did not load synchronously."
                        << "URL:" << url.toString()
                        << "(original URL:" << originalUrl.toString() << ")";
        } else {
            emit logErrors(component->errors());
            delete m_object;
            if (m_fallbackView)
                showErrorScreen();
        }
    } else if (QQuickWindow *window = qobject_cast<QQuickWindow *>(m_object)) {
        // TODO (why) is this needed?
        m_qmlEngine->setIncubationController(window->incubationController());
        m_activeWindow = window;
    } else if (QQuickItem *item = qobject_cast<QQuickItem *>(m_object)) {
        if (m_fallbackView) {
            const bool hasEmptySize = QSize(item->width(), item->height()).isEmpty();
            if ((m_activePlugin && m_activePlugin->isFullScreen()) || hasEmptySize)
                m_fallbackView->setResizeMode(QQuickView::SizeRootObjectToView);
            else
                m_fallbackView->setResizeMode(QQuickView::SizeViewToRootObject);
            component->setParent(m_fallbackView);
            m_fallbackView->setContent(url, component.take(), m_object);
            m_activeWindow = m_fallbackView;
        } else {
            logError(tr("LiveNodeEngine: Cannot display this component: "
                        "Root object is not a QQuickWindow and no LiveNodeEngine::fallbackView set."));
        }
    } else {
        logError(tr("LiveNodeEngine: Cannot display this component: "
                    "Root object is not a QQuickWindow nor a QQuickItem."));
        if (m_fallbackView)
            showErrorScreen();
    }

    if (m_activeWindow) {
        m_activeWindowConnections << connect(m_activeWindow.data(), &QWindow::widthChanged,
                                             this, &LiveNodeEngine::onSizeChanged);
        m_activeWindowConnections << connect(m_activeWindow.data(), &QWindow::heightChanged,
                                             this, &LiveNodeEngine::onSizeChanged);
        onSizeChanged();
    }

    emit documentLoaded();
    emit activeWindowChanged(m_activeWindow);

    if (m_fallbackView && m_fallbackView != m_activeWindow)
        m_fallbackView->close();

    // Delay showing the window after activeWindowChanged is handled by
    // WindowWidget::setHostedWindow() - it would be destroyed there anyway.
    // (Applies when this is instantiated for the bench.)
    if (m_activeWindow)
        m_activeWindow->show();
}

/*!
 * Updates \a content of the given workspace \a document when enabled.
 *
 * The behavior of this function is controlled by WorkspaceOptions passed to setWorkspace().
 */
void LiveNodeEngine::updateDocument(const LiveDocument &document, const QByteArray &content)
{
    if (QFileInfo(document.relativeFilePath()).suffix() == QLatin1String("qrc")) {
        QBuffer buffer;
        buffer.setData(content);
        buffer.open(QIODevice::ReadOnly);
        if (!m_resourceMap->updateMapping(document, &buffer))
            qWarning() << "Unable to parse qrc file " << document.relativeFilePath() << ":" << m_resourceMap->errorString();
    }

    if (!(m_workspaceOptions & AllowUpdates)) {
        return;
    }

    bool existsInWorkspace = document.existsIn(m_workspace);
    bool mapsToResource = document.mapsToResource(*m_resourceMap);
    if (!existsInWorkspace && !mapsToResource && !(m_workspaceOptions & AllowCreateMissing))
        return;

    bool useOverlay = (m_workspaceOptions & UpdatesAsOverlay) || mapsToResource;

    QString writablePath = useOverlay
        ? m_overlay->reserve(document, existsInWorkspace)
        : document.absoluteFilePathIn(m_workspace);

    QString writableDirPath = QFileInfo(writablePath).absoluteDir().absolutePath();
    QDir().mkpath(writableDirPath);
    QFile file(writablePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to save file: " << file.errorString();
        return;
    }
    file.write(content);
    file.close();

    if (!m_activeFile.isNull())
        delayReload();
}


/*!
 * Allows to adapt a \a url to display not native QML documents (e.g. images).
 */
QUrl LiveNodeEngine::queryDocumentViewer(const QUrl& url)
{
    initPlugins();

    foreach (ContentAdapterInterface *adapter, m_plugins) {
        if (adapter->canAdapt(url)) {
            adapter->cleanUp();
            adapter->setAvailableFeatures(m_quickFeatures);

            m_activePlugin = adapter;

            return adapter->adapt(url, m_qmlEngine->rootContext());
        }
    }

    m_activePlugin = 0;

    return url;
}

/*!
 * Returns the current workspace path.
 */
QString LiveNodeEngine::workspace() const
{
    return m_workspace.absolutePath();
}

/*!
 * Sets the current workspace to \a path. Documents location will be adjusted based on
 * this workspace path. Certain features can be controled by passing \a options.
 *
 * \sa WorkspaceOptions
 */
void LiveNodeEngine::setWorkspace(const QString &path, WorkspaceOptions options)
{
    Q_ASSERT(qmlEngine());

    m_workspace = QDir(path);
    m_workspaceOptions = options;

    if (m_workspaceOptions & LoadDummyData)
        QmlHelper::loadDummyData(m_qmlEngine, m_workspace.absolutePath());

    if ((m_workspaceOptions & UpdatesAsOverlay) && !(m_workspaceOptions & AllowUpdates)) {
        qWarning() << "Got UpdatesAsOverlay without AllowUpdates. Enabling AllowUpdates.";
        m_workspaceOptions |= AllowUpdates;
    }

    if ((m_workspaceOptions & AllowCreateMissing) && !(m_workspaceOptions & AllowUpdates)) {
        qWarning() << "Got AllowCreateMissing without AllowUpdates. Enabling AllowUpdates.";
        m_workspaceOptions |= AllowUpdates;
    }

    if (m_workspaceOptions & AllowUpdates) {
        // Even without UpdatesAsOverlay the overlay is used for Qt resources
        m_overlay = new Overlay(m_workspace.path(), this);
        m_urlInterceptor = new UrlInterceptor(m_workspace, m_overlay, m_resourceMap, qmlEngine()->urlInterceptor(), this);
        qmlEngine()->setUrlInterceptor(m_urlInterceptor);
    }

    emit workspaceChanged(workspace());
}

/*!
 * Returns the ResourceMap managed by this instance.
 *
 * \sa LiveDocument
 */
ResourceMap *LiveNodeEngine::resourceMap() const
{
    return m_resourceMap;
}

/*!
 * Sets the pluginPath to \a path.
 *
 * The pluginPath will be used to load QmlLive plugins
 * \sa {ContentPlugin Example}
 */
void LiveNodeEngine::setPluginPath(const QString &path)
{
    m_pluginFactory->setPluginPath(path);
}

/*!
 * Returns the current pluginPath
 */
QString LiveNodeEngine::pluginPath() const
{
    return m_pluginFactory->pluginPath();
}

/*!
 * Returns the current active document.
 */
LiveDocument LiveNodeEngine::activeDocument() const
{
    return m_activeFile;
}

/*!
 * Returns the active content adapter plugin
 */
ContentAdapterInterface *LiveNodeEngine::activePlugin() const
{
    return m_activePlugin;
}

/*!
 * Returns the current active window.
 * \sa activeWindowChanged()
 */
QQuickWindow *LiveNodeEngine::activeWindow() const
{
    return m_activeWindow;
}

/*!
 * Loads all plugins found in the Pluginpath
 */
void LiveNodeEngine::initPlugins()
{
    if (m_plugins.isEmpty()) {
        m_pluginFactory->load();
        m_plugins.append(m_pluginFactory->plugins());
        m_plugins.append(new ImageAdapter(this));
        m_plugins.append(new FontAdapter(this));
    }
}

/*!
 * Handles size changes and updates the view according
 */
void LiveNodeEngine::onSizeChanged()
{
    Q_ASSERT(m_activeWindow != 0);

    if (m_activeWindow->width() != -1 && m_activeWindow->height() != -1) {
        m_runtime->setScreenWidth(m_activeWindow->width());
        m_runtime->setScreenHeight(m_activeWindow->height());
    }

    setRotation(m_rotation);
}

/*!
 * \fn void LiveNodeEngine::activeDocumentChanged(const LiveDocument& document)
 *
 * The document \a document was loaded with loadDocument() and is now the
 * activeDocument(). This signal is only emitted when the new document differs
 * from the previously loaded one.
 *
 * \sa documentLoaded()
 */

/*!
 * \fn void LiveNodeEngine::clearLog()
 *
 * Requested to clear the log
 */

/*!
 * \fn void LiveNodeEngine::logIgnoreMessages(bool)
 *
 * Requsted to ignore the Messages when \a on is true
 */

/*!
 * \fn void LiveNodeEngine::documentLoaded()
 *
 * The signal is emitted when the document has finished loading.
 */

/*!
 * \fn void LiveNodeEngine::activeWindowChanged(QQuickWindow *window)
 *
 * The signal is emitted when the activeWindow has changed by changing or
 * reloading the document. \a window is the newly activated window.
 */

/*!
 * \fn void LiveNodeEngine::logErrors(const QList<QQmlError> &errors)
 *
 * Log the Errors \a errors
 */

/*!
 * \fn void LiveNodeEngine::workspaceChanged(const QString &workspace)
 *
 * This signal is emitted after workspace is changed with setWorkspace(). \a
 * workspace is the new workspace path.
 *
 * \sa workspace()
 */

#include "livenodeengine.moc"
