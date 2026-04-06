#include <QTest>
#include <QApplication>
#include "ui/AvatarWidget.h"

using namespace CollabText::Ui;

class TestIdentityWidgets : public QObject {
    Q_OBJECT

private slots:
    void avatar_widget_default_size() {
        AvatarWidget w;
        QCOMPARE(w.sizeHint(), QSize(40, 40));
    }

    void avatar_widget_initials_fallback() {
        AvatarWidget w;
        w.setIdentity("Clinton Selke", "#3b82f6");
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void avatar_widget_set_image() {
        AvatarWidget w;
        std::vector<uint8_t> data = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
            0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
            0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
            0x00, 0x00, 0x02, 0x00, 0x01, 0xE2, 0x21, 0xBC,
            0x33, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
            0x44, 0xAE, 0x42, 0x60, 0x82,
        };
        w.setImage(data);
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void avatar_widget_clear_image_reverts_to_initials() {
        AvatarWidget w;
        w.setIdentity("Alice", "#22c55e");
        w.setImage({0x89, 0x50, 0x4E, 0x47});
        w.clearImage();
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }
};

QTEST_MAIN(TestIdentityWidgets)
#include "tst_identity_widgets.moc"
