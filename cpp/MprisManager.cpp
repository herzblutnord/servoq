/*
 * Copyright (c) 2026, ServoQ contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "MprisManager.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QMainWindow>
#include <QWidget>

#include <algorithm>

namespace ServoQ {

static constexpr char const* kObjectPath = "/org/mpris/MediaPlayer2";
static constexpr char const* kPlayerInterface = "org.mpris.MediaPlayer2.Player";

// ---- MediaPlayer2Adaptor (root) -----------------------------------------

MediaPlayer2Adaptor::MediaPlayer2Adaptor(MprisManager* manager)
    : QDBusAbstractAdaptor(manager)
    , m_manager(manager)
{
}

QStringList MediaPlayer2Adaptor::supportedUriSchemes() const
{
    return { QStringLiteral("http"), QStringLiteral("https") };
}

void MediaPlayer2Adaptor::Raise()
{
    m_manager->raiseWindow();
}

void MediaPlayer2Adaptor::Quit()
{
    m_manager->quit();
}

// ---- MediaPlayer2PlayerAdaptor ------------------------------------------

MediaPlayer2PlayerAdaptor::MediaPlayer2PlayerAdaptor(MprisManager* manager)
    : QDBusAbstractAdaptor(manager)
    , m_manager(manager)
{
}

QString MediaPlayer2PlayerAdaptor::playbackStatus() const { return m_manager->playbackStatus(); }
QVariantMap MediaPlayer2PlayerAdaptor::metadata() const { return m_manager->metadata(); }
qlonglong MediaPlayer2PlayerAdaptor::position() const { return m_manager->position(); }
bool MediaPlayer2PlayerAdaptor::canGoNext() const { return m_manager->canGoNext(); }
bool MediaPlayer2PlayerAdaptor::canGoPrevious() const { return m_manager->canGoPrevious(); }

// MediaSessionActionType codes mirror src/bridge.rs media_session_action:
// 0 Play, 1 Pause, 2 SeekBackward, 3 SeekForward, 4 PreviousTrack,
// 5 NextTrack, 6 SkipAd, 7 Stop, 8 SeekTo.
void MediaPlayer2PlayerAdaptor::Play() { m_manager->dispatchAction(0); }
void MediaPlayer2PlayerAdaptor::Pause() { m_manager->dispatchAction(1); }
void MediaPlayer2PlayerAdaptor::PlayPause()
{
    m_manager->dispatchAction(m_manager->playbackStatus() == QStringLiteral("Playing") ? 1 : 0);
}
void MediaPlayer2PlayerAdaptor::Stop() { m_manager->dispatchAction(7); }
void MediaPlayer2PlayerAdaptor::Next() { m_manager->dispatchAction(5); }
void MediaPlayer2PlayerAdaptor::Previous() { m_manager->dispatchAction(4); }
void MediaPlayer2PlayerAdaptor::Seek(qlonglong offset)
{
    // The Servo media-session action enum carries no target, so map a relative
    // seek to the short forward/backward intents.
    m_manager->dispatchAction(offset >= 0 ? 3 : 2);
}
void MediaPlayer2PlayerAdaptor::SetPosition(QDBusObjectPath const&, qlonglong)
{
    m_manager->dispatchAction(8); // SeekTo (best effort; no precise target available)
}
void MediaPlayer2PlayerAdaptor::OpenUri(QString const&) {}

// ---- MprisManager -------------------------------------------------------

MprisManager* MprisManager::the()
{
    static MprisManager* instance = new MprisManager();
    return instance;
}

MprisManager::MprisManager()
    : QObject(QCoreApplication::instance())
{
    m_root_adaptor = new MediaPlayer2Adaptor(this);
    m_player_adaptor = new MediaPlayer2PlayerAdaptor(this);
}

void MprisManager::ensureRegistered()
{
    if (m_registered)
        return;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    if (!bus.registerObject(QString::fromLatin1(kObjectPath), this))
        return;

    // The well-known name is shared; a second instance falls back to a unique
    // suffix per the MPRIS spec.
    QString service = QStringLiteral("org.mpris.MediaPlayer2.servoq");
    if (!bus.registerService(service)) {
        service += QStringLiteral(".instance%1").arg(QCoreApplication::applicationPid());
        bus.registerService(service);
    }
    m_registered = true;
}

void MprisManager::handleEvent(int tab_id, int kind, int playback_state,
    QString const& title, QString const& artist, QString const& album,
    double duration, double position, double playback_rate)
{
    ensureRegistered();
    m_media_tab_id = tab_id;

    QStringList changed;
    switch (kind) {
    case 0: // SetMetadata
        m_title = title;
        m_artist = artist;
        m_album = album;
        ++m_track_serial;
        changed << QStringLiteral("Metadata");
        break;
    case 1: { // PlaybackStateChange
        QString status = playback_state == 2 ? QStringLiteral("Playing")
            : playback_state == 3            ? QStringLiteral("Paused")
                                             : QStringLiteral("Stopped");
        if (status != m_playback_status) {
            m_playback_status = status;
            changed << QStringLiteral("PlaybackStatus");
        }
        // Re-base the position clock so extrapolation tracks the new state.
        m_position_base_secs = position > 0.0 ? position : m_position_base_secs;
        m_position_timer.restart();
        break;
    }
    case 2: // SetPositionState
        m_duration_secs = duration;
        m_position_base_secs = position;
        m_playback_rate = playback_rate != 0.0 ? playback_rate : 1.0;
        m_position_timer.restart();
        changed << QStringLiteral("Metadata"); // mpris:length lives in Metadata
        if (m_player_adaptor)
            emit m_player_adaptor->Seeked(this->position());
        break;
    default:
        return;
    }

    if (!changed.isEmpty())
        emitPropertiesChanged(changed);
}

void MprisManager::onTabClosed(int tab_id)
{
    if (tab_id != m_media_tab_id)
        return;
    m_media_tab_id = -1;
    if (m_playback_status != QStringLiteral("Stopped")) {
        m_playback_status = QStringLiteral("Stopped");
        emitPropertiesChanged({ QStringLiteral("PlaybackStatus") });
    }
}

QString MprisManager::playbackStatus() const { return m_playback_status; }

QVariantMap MprisManager::metadata() const
{
    QVariantMap map;
    // mpris:trackid must be a valid object path even when nothing is playing.
    map.insert(QStringLiteral("mpris:trackid"),
        QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/servoq/track/%1").arg(m_track_serial))));
    if (m_duration_secs > 0.0)
        map.insert(QStringLiteral("mpris:length"), static_cast<qlonglong>(m_duration_secs * 1e6));
    map.insert(QStringLiteral("xesam:title"),
        m_title.isEmpty() ? QStringLiteral("ServoQ") : m_title);
    if (!m_artist.isEmpty())
        map.insert(QStringLiteral("xesam:artist"), QStringList { m_artist });
    if (!m_album.isEmpty())
        map.insert(QStringLiteral("xesam:album"), m_album);
    return map;
}

qlonglong MprisManager::position() const
{
    double secs = m_position_base_secs;
    if (m_playback_status == QStringLiteral("Playing") && m_position_timer.isValid())
        secs += (m_position_timer.elapsed() / 1000.0) * m_playback_rate;
    if (m_duration_secs > 0.0)
        secs = std::min(secs, m_duration_secs);
    return static_cast<qlonglong>(std::max(0.0, secs) * 1e6);
}

void MprisManager::dispatchAction(int action_code)
{
    if (m_media_tab_id < 0)
        return;
    servoq::media_session_action(m_media_tab_id, action_code);
}

void MprisManager::raiseWindow()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<QMainWindow*>(widget)) {
            window->showNormal();
            window->raise();
            window->activateWindow();
            return;
        }
    }
}

void MprisManager::quit()
{
    QCoreApplication::quit();
}

void MprisManager::emitPropertiesChanged(QStringList const& changed_properties)
{
    if (!m_registered)
        return;
    QVariantMap props;
    for (auto const& name : changed_properties) {
        if (name == QStringLiteral("PlaybackStatus"))
            props.insert(name, playbackStatus());
        else if (name == QStringLiteral("Metadata"))
            props.insert(name, metadata());
    }
    auto msg = QDBusMessage::createSignal(QString::fromLatin1(kObjectPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
    msg << QString::fromLatin1(kPlayerInterface) << props << QStringList {};
    QDBusConnection::sessionBus().send(msg);
}

}

// ---- Rust -> C++ callback (servoq::notify_media_session_event) ----------

namespace servoq {

void notify_media_session_event(::std::int32_t tab_id, ::std::int32_t kind,
    ::std::int32_t playback_state, ::rust::Str title, ::rust::Str artist,
    ::rust::Str album, double duration, double position, double playback_rate)
{
    ServoQ::MprisManager::the()->handleEvent(
        static_cast<int>(tab_id), static_cast<int>(kind), static_cast<int>(playback_state),
        QString::fromUtf8(title.data(), static_cast<int>(title.size())),
        QString::fromUtf8(artist.data(), static_cast<int>(artist.size())),
        QString::fromUtf8(album.data(), static_cast<int>(album.size())),
        duration, position, playback_rate);
}

}
