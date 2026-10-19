#include "Document.h"
#include "CollabPane.h"

#include "crdt/SidecarManifest.h"
#include <collabtext/DocumentSession.h>

#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QUuid>

#include <chrono>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;
using namespace CollabText::Crdt;

namespace CollabEdit {

namespace {

std::string now_iso8601_utc() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

QString readFileToString(const QString &path, QString *err) {
    QFile f(path);
    if (!f.exists()) return QString();
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = QObject::tr("cannot read %1: %2").arg(path, f.errorString());
        return QString();
    }
    QTextStream in(&f);
    return in.readAll();
}

QString writeStringToFile(const QString &path, const QString &content) {
    QSaveFile tmp(path);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Text))
        return QObject::tr("cannot write %1: %2").arg(path, tmp.errorString());
    QTextStream out(&tmp);
    out << content;
    out.flush();
    if (out.status() != QTextStream::Ok)
        return QObject::tr("cannot write %1: %2").arg(path, tmp.errorString());
    if (!tmp.commit())
        return QObject::tr("cannot replace %1: %2").arg(path, tmp.errorString());
    return {};
}

} // namespace

Document::Document(CollabText::Identity::Identity identity,
                   std::string replica_name,
                   QObject *parent,
                   fs::path private_state_root)
    : QObject(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
    , m_privateStateRoot(std::move(private_state_root))
{
    if (m_privateStateRoot.empty()) {
        const QString appData = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
        m_privateStateRoot = fs::path(appData.toStdString()) / "replica-state";
    }
    auto *lbl = new QLabel(tr("(no document open — use File → New or Open...)"));
    lbl->setAlignment(Qt::AlignCenter);
    m_emptyWidget = lbl;
    m_currentWidget = m_emptyWidget;
}

Document::~Document() {
    if (m_collabPane) {
        const auto error = m_collabPane->shutdown();
        if (!error.isEmpty()) qWarning("collabtext: %s", qPrintable(error));
    }
    delete m_plainEdit;
    delete m_collabPane;
    delete m_emptyWidget;
}

QWidget *Document::widget() const { return m_currentWidget; }

QString Document::displayName() const {
    if (m_path.isEmpty()) return tr("(untitled)");
    return QFileInfo(m_path).fileName();
}

QString Document::path() const { return m_path; }
bool Document::isCollab() const { return m_collab; }

bool Document::isModified() const {
    if (m_collabPane) return m_collabPane->isModified();
    if (m_plainEdit) return m_plainEdit->document()->isModified();
    return false;
}

fs::path Document::sidecarPath(const QString &filePath) const {
    return fs::path(filePath.toStdString() + ".collab");
}

QString Document::newDoc() {
    const auto closeError = closeDoc();
    if (!closeError.isEmpty()) return closeError;
    m_path.clear();
    m_collab = false;
    m_plainEdit = new QPlainTextEdit;
    m_currentWidget = m_plainEdit;
    QObject::connect(m_plainEdit->document(), &QTextDocument::contentsChanged,
                     this, &Document::emitChanged);
    emit changed();
    return {};
}

QString Document::open(const QString &path) {
    const auto closeError = closeDoc();
    if (!closeError.isEmpty()) return closeError;

    auto sidecar = sidecarPath(path);
    if (fs::exists(sidecar / "manifest.json")) {
        m_path = path;
        QString error = openInCollabMode(path);
        if (!error.isEmpty()) m_path.clear();
        return error;
    }
    m_path = path;
    QString error = openInPlainMode(path);
    if (!error.isEmpty()) m_path.clear();
    return error;
}

QString Document::openInPlainMode(const QString &path) {
    QString err;
    QString content = readFileToString(path, &err);
    if (!err.isEmpty()) return err;

    m_collab = false;
    m_plainEdit = new QPlainTextEdit;
    m_plainEdit->setPlainText(content);
    m_plainEdit->document()->setModified(false);
    m_currentWidget = m_plainEdit;
    QObject::connect(m_plainEdit->document(), &QTextDocument::contentsChanged,
                     this, &Document::emitChanged);
    emit changed();
    return {};
}

QString Document::openInCollabMode(const QString &path) {
    auto sidecar = sidecarPath(path);
    QString materializedError;
    const QString materializedView = readFileToString(path, &materializedError);
    if (!materializedError.isEmpty()) return materializedError;
    auto manifest = read_manifest(sidecar / "manifest.json");
    if (!manifest)
        return tr("Sidecar exists but manifest is invalid: %1")
                   .arg(QString::fromStdString((sidecar / "manifest.json").string()));

    for (const auto &entry : fs::directory_iterator(sidecar)) {
        std::string name = entry.path().filename().string();
        if (name.find("seed.sync-conflict") != std::string::npos
            || name.find("manifest.sync-conflict") != std::string::npos) {
            return tr("Enrollment conflict: extra '%1' in sidecar. "
                      "Resolve manually before opening.")
                       .arg(QString::fromStdString(name));
        }
    }

    std::ifstream sf(sidecar / "seed.txt", std::ios::binary);
    if (!sf) return tr("Sidecar missing seed.txt");
    std::string seed((std::istreambuf_iterator<char>(sf)),
                      std::istreambuf_iterator<char>());
    if (CollabText::sha256_hex(seed) != manifest->seed_sha256)
        return tr("seed.txt SHA mismatch — sidecar may be corrupt");

    CollabText::DocumentSessionConfig config;
    config.shared_store = sidecar;
    config.private_state = m_privateStateRoot / manifest->doc_id;
    config.document = manifest->doc_id;
    config.stream = "text";
    config.epoch = 1;
    config.initial_text = seed;
    auto opened = CollabText::DocumentSession::open(std::move(config));
    if (!opened.ok())
        return tr("Cannot recover collaborative document: %1")
            .arg(QString::fromUtf8(opened.error().message().data(),
                                   static_cast<qsizetype>(opened.error().message().size())));

    const bool viewNeedsSave =
        QString::fromStdString(opened.value()->text()) != materializedView;
    m_collab = true;
    m_collabPane = new CollabPane(m_identity, m_replicaName,
                                  sidecar, std::move(opened).value());
    connect(m_collabPane, &CollabPane::changed,
            this, &Document::emitChanged);
    connect(m_collabPane, &CollabPane::errorOccurred,
            this, [this](const QString &message) { emit errorOccurred(message); });
    if (viewNeedsSave) m_collabPane->markModified();
    m_currentWidget = m_collabPane;
    emit changed();
    return {};
}

QString Document::save() {
    if (m_collab && m_collabPane) {
        if (m_path.isEmpty()) return tr("no file path");
        QString err = m_collabPane->flush();
        if (!err.isEmpty()) return err;
        QString content = QString::fromStdString(m_collabPane->text());
        err = writeStringToFile(m_path, content);
        if (err.isEmpty()) {
            m_collabPane->markSaved();
            emit changed();
        }
        return err;
    }
    if (m_plainEdit) {
        if (m_path.isEmpty()) return tr("no file path; use Save As");
        QString err = writeStringToFile(m_path, m_plainEdit->toPlainText());
        if (err.isEmpty()) {
            m_plainEdit->document()->setModified(false);
            emit changed();
        }
        return err;
    }
    return tr("no document");
}

QString Document::saveAs(const QString &path) {
    if (m_collab) return tr("Save As is disabled in Collab mode");
    if (!m_plainEdit) return tr("no document");
    const QString previousPath = m_path;
    m_path = path;
    const QString error = save();
    if (!error.isEmpty()) m_path = previousPath;
    return error;
}

QString Document::enableCollab() {
    if (m_collab) return tr("already in Collab mode");
    if (!m_plainEdit) return tr("no document");
    if (m_path.isEmpty()) return tr("save the file first (need a path)");

    QString text = m_plainEdit->toPlainText();
    QString err = writeStringToFile(m_path, text);
    if (!err.isEmpty()) return err;

    auto sidecar = sidecarPath(m_path);
    std::error_code ec;
    fs::create_directories(sidecar, ec);
    if (ec) return tr("cannot create sidecar: %1").arg(QString::fromStdString(ec.message()));

    std::string seedStr = text.toStdString();
    auto seedPath = sidecar / "seed.txt";
    bool needWriteSeed = true;
    if (fs::exists(seedPath)) {
        std::ifstream sf(seedPath, std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(sf)),
                              std::istreambuf_iterator<char>());
        if (existing == seedStr) needWriteSeed = false;
    }
    if (needWriteSeed) {
        std::ofstream sf(seedPath, std::ios::binary | std::ios::trunc);
        if (!sf) return tr("cannot write seed.txt");
        sf.write(seedStr.data(), static_cast<std::streamsize>(seedStr.size()));
    }

    SidecarManifest manifest;
    manifest.schema_version = 4;
    manifest.doc_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    manifest.enrolled_at = now_iso8601_utc();
    manifest.original_filename = QFileInfo(m_path).fileName().toStdString();
    manifest.seed_sha256 = CollabText::sha256_hex(seedStr);
    try {
        write_manifest(sidecar / "manifest.json", manifest);
    } catch (const std::exception &e) {
        return tr("cannot write manifest: %1").arg(e.what());
    }

    err = openInCollabMode(m_path);
    if (!err.isEmpty()) return err;
    if (m_plainEdit) {
        m_plainEdit->deleteLater();
        m_plainEdit = nullptr;
    }
    return {};
}

QString Document::closeDoc() {
    if (m_collabPane) {
        const auto error = m_collabPane->shutdown();
        if (!error.isEmpty()) return error;
        m_collabPane->deleteLater();
        m_collabPane = nullptr;
    }
    if (m_plainEdit) {
        m_plainEdit->deleteLater();
        m_plainEdit = nullptr;
    }
    m_path.clear();
    m_collab = false;
    m_currentWidget = m_emptyWidget;
    emit changed();
    return {};
}

} // namespace CollabEdit
