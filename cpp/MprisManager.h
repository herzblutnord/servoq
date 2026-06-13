/*
 * Copyright (c) 2026, ServoQ contributors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * MPRIS (Media Player Remote Interfacing Specification) bridge for ServoQ
 * (M5.6). Exposes the page Media Session over D-Bus so desktop media controls
 * (GNOME/KDE shells, playerctl, headset keys) can see and drive web playback.
 *
 * Web pages report playback through Servo's MediaSessionEvent delegate, which
 * ServoQ forwards via servoq::notify_media_session_event into
 * MprisManager::handleEvent. Control actions from the desktop are dispatched
 * back to the page through servoq::media_session_action.
 */
#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace ServoQ {

class MprisManager;

// org.mpris.MediaPlayer2 — the root media-player interface.
class MediaPlayer2Adaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MediaPlayer2Adaptor(MprisManager* manager);

    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("ServoQ"); }
    QString desktopEntry() const { return QStringLiteral("servoq"); }
    QStringList supportedUriSchemes() const;
    QStringList supportedMimeTypes() const { return {}; }

public slots:
    void Raise();
    void Quit();

private:
    MprisManager* m_manager;
};

// org.mpris.MediaPlayer2.Player — playback state and transport controls.
class MediaPlayer2PlayerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    explicit MediaPlayer2PlayerAdaptor(MprisManager* manager);

    QString playbackStatus() const;
    QVariantMap metadata() const;
    double rate() const { return 1.0; }
    void setRate(double) {}
    double minimumRate() const { return 1.0; }
    double maximumRate() const { return 1.0; }
    double volume() const { return 1.0; }
    void setVolume(double) {}
    qlonglong position() const;
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const { return true; }
    bool canPause() const { return true; }
    bool canSeek() const { return true; }
    bool canControl() const { return true; }

public slots:
    void Play();
    void Pause();
    void PlayPause();
    void Stop();
    void Next();
    void Previous();
    void Seek(qlonglong offset);
    void SetPosition(QDBusObjectPath const& trackId, qlonglong position);
    void OpenUri(QString const& uri);

signals:
    void Seeked(qlonglong position);

private:
    MprisManager* m_manager;
};

class MprisManager final : public QObject {
    Q_OBJECT
public:
    static MprisManager* the();

    // Forwarded from servoq::notify_media_session_event (main thread).
    // kind: 0 SetMetadata, 1 PlaybackStateChange, 2 SetPositionState.
    void handleEvent(int tab_id, int kind, int playback_state,
        QString const& title, QString const& artist, QString const& album,
        double duration, double position, double playback_rate);

    // Called when a tab closes so a stale media session stops controlling the
    // desktop player.
    void onTabClosed(int tab_id);

    // State accessors used by the adaptors.
    QString playbackStatus() const;
    QVariantMap metadata() const;
    qlonglong position() const;
    bool canGoNext() const { return m_can_go_next; }
    bool canGoPrevious() const { return m_can_go_previous; }

    // Control dispatch from the desktop back to the page's media session.
    void dispatchAction(int action_code);
    void raiseWindow();
    void quit();

private:
    MprisManager();

    void ensureRegistered();
    void emitPropertiesChanged(QStringList const& changed_properties);

    bool m_registered { false };
    MediaPlayer2Adaptor* m_root_adaptor { nullptr };
    MediaPlayer2PlayerAdaptor* m_player_adaptor { nullptr };

    int m_media_tab_id { -1 };
    QString m_playback_status { QStringLiteral("Stopped") };
    QString m_title;
    QString m_artist;
    QString m_album;
    qint64 m_track_serial { 0 };

    double m_duration_secs { 0.0 };
    double m_position_base_secs { 0.0 };
    QElapsedTimer m_position_timer;
    double m_playback_rate { 1.0 };

    bool m_can_go_next { true };
    bool m_can_go_previous { true };
};

}
