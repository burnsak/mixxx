#include "library/banshee/bansheefeature.h"

#include <QList>
#include <QMessageBox>
#include <QtDebug>

#include "library/banshee/bansheedbconnection.h"
#include "library/banshee/bansheeplaylistmodel.h"
#include "library/baseexternalplaylistmodel.h"
#include "library/dao/settingsdao.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "moc_bansheefeature.cpp"
#include "track/track.h"

const QString BansheeFeature::BANSHEE_MOUNT_KEY = "mixxx.BansheeFeature.mount";
QString BansheeFeature::m_databaseFile;

BansheeFeature::BansheeFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("banshee")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_cancelImport(false) {
    Q_UNUSED(pConfig);
    m_pBansheePlaylistModel = new BansheePlaylistModel(
            this, m_pLibrary->trackCollectionManager(), &m_connection);
    m_isActivated = false;
    m_title = tr("Banshee");
}

BansheeFeature::~BansheeFeature() {
    qDebug() << "~BansheeFeature()";
    // stop import thread, if still running
    m_cancelImport = true;
    if (m_future.isRunning()) {
        qDebug() << "m_future still running";
        m_future.waitForFinished();
        qDebug() << "m_future finished";
    }

    delete m_pBansheePlaylistModel;
}

// static
bool BansheeFeature::isSupported() {
    return !m_databaseFile.isEmpty();
}

// static
void BansheeFeature::prepareDbPath(UserSettingsPointer pConfig) {
    m_databaseFile = pConfig->getValueString(ConfigKey("[Banshee]","Database"));
    if (!QFile::exists(m_databaseFile)) {
        // Fall back to default
        m_databaseFile = BansheeDbConnection::getDatabaseFile();
    }
}

QVariant BansheeFeature::title() {
    return m_title;
}

void BansheeFeature::activate() {
    //qDebug("BansheeFeature::activate()");

    if (!m_isActivated) {
        if (!QFile::exists(m_databaseFile)) {
            // Fall back to default
            m_databaseFile = BansheeDbConnection::getDatabaseFile();
        }

        if (!QFile::exists(m_databaseFile)) {
            QMessageBox::warning(
                    nullptr,
                    tr("Error loading Banshee database"),
                    tr("Banshee database file not found at\n") +
                            m_databaseFile);
            qDebug() << m_databaseFile << "does not exist";
        }

        mixxx::FileInfo fileInfo(m_databaseFile);
        if (!Sandbox::askForAccess(&fileInfo) ||
                !m_connection.open(m_databaseFile)) {
            QMessageBox::warning(
                    nullptr,
                    tr("Error loading Banshee database"),
                    tr("There was an error loading your Banshee database at\n") +
                            m_databaseFile);
            return;
        }

        qDebug() << "Using Banshee Database Schema V" << m_connection.getSchemaVersion();

        m_isActivated =  true;

        std::unique_ptr<TreeItem> pRootItem = TreeItem::newRoot(this);
        QList<BansheeDbConnection::Playlist> playlists = m_connection.getPlaylists();
        for (const BansheeDbConnection::Playlist& playlist: playlists) {
            qDebug() << playlist.name;
            // append the playlist to the child model
            pRootItem->appendChild(playlist.name, playlist.playlistId);
        }
        m_pSidebarModel->setRootItem(std::move(pRootItem));

        if (m_isActivated) {
            activate();
        }
        qDebug() << "Banshee library loaded: success";

        //calls a slot in the sidebarmodel such that 'isLoading' is removed from the feature title.
        m_title = tr("Banshee");
        emit featureLoadingFinished(this);
    }

    m_pBansheePlaylistModel->selectPlaylist(0); // Loads the master playlist
    emit showTrackModel(m_pBansheePlaylistModel);
    emit enableCoverArtDisplay(false);
}

void BansheeFeature::activateChild(const QModelIndex& index) {
    TreeItem *item = static_cast<TreeItem*>(index.internalPointer());
    int playlistID = item->getData().toInt();
    if (playlistID > 0) {
        qDebug() << "Activating " << item->getLabel();
        m_pBansheePlaylistModel->selectPlaylist(playlistID);
        emit showTrackModel(m_pBansheePlaylistModel);
        emit enableCoverArtDisplay(false);
    }
}

TreeItemModel* BansheeFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void BansheeFeature::appendTrackIdsFromRightClickIndex(QList<TrackId>* trackIds, QString* pPlaylist) {
    if (lastRightClickedIndex().isValid()) {
        TreeItem *item = static_cast<TreeItem*>(lastRightClickedIndex().internalPointer());
        *pPlaylist = item->getLabel();
        int playlistID = item->getData().toInt();
        qDebug() << "BansheeFeature::appendTrackIdsFromRightClickIndex " << *pPlaylist << " " << playlistID;
        if (playlistID > 0) {
            BansheePlaylistModel* pPlaylistModelToAdd =
                    new BansheePlaylistModel(this,
                            m_pLibrary->trackCollectionManager(),
                            &m_connection);
            pPlaylistModelToAdd->selectPlaylist(playlistID);
            pPlaylistModelToAdd->select();

            // Copy Tracks
            int rows = pPlaylistModelToAdd->rowCount();
            for (int i = 0; i < rows; ++i) {
                QModelIndex index = pPlaylistModelToAdd->index(i,0);
                if (index.isValid()) {
                    //qDebug() << pPlaylistModelToAdd->getTrackUrl(index);
                    TrackPointer track = pPlaylistModelToAdd->getTrack(index);
                    trackIds->append(track->getId());
                }
            }
            delete pPlaylistModelToAdd;
        }
    }
}
