#include "MainWindow.h"

#include "collabtext/IdentityStore.h"
#include "ui/IdentitySetupDialog.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>
#include <QSysInfo>

#include <string>

using namespace CollabText;

static std::string deriveReplicaName(const Identity::Identity &id) {
    std::string idPrefix = id.identity_id.size() >= 8
        ? id.identity_id.substr(0, 8)
        : id.identity_id;
    std::string host = QSysInfo::machineHostName().toStdString();
    if (host.empty()) host = "host";
    return idPrefix + "-" + host;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("collabedit");
    QApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Minimal CRDT collaborative editor.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "File to open (optional).");
    parser.process(app);

    QString configRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configRoot);
    Identity::IdentityStore store(configRoot.toStdString());

    auto loaded = store.load();
    Identity::Identity identity;
    if (loaded) {
        identity = *loaded;
    } else {
        Ui::IdentitySetupDialog dlg(store);
        if (dlg.exec() != QDialog::Accepted) return 0;
        identity = dlg.identity();
    }

    std::string replicaName = deriveReplicaName(identity);

    CollabEdit::MainWindow win(identity, replicaName);
    win.show();

    auto args = parser.positionalArguments();
    if (!args.isEmpty()) {
        win.openFile(args.first());
    }

    return app.exec();
}
