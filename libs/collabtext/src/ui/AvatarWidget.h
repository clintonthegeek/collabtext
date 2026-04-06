#pragma once

#include <QWidget>
#include <QPixmap>
#include <string>
#include <vector>

namespace CollabText::Ui {

class AvatarWidget : public QWidget {
    Q_OBJECT
public:
    explicit AvatarWidget(QWidget *parent = nullptr);

    void setIdentity(const std::string &display_name, const std::string &color);
    void setImage(const std::vector<uint8_t> &data);
    void clearImage();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString extractInitials(const QString &name) const;

    QString m_displayName;
    QColor m_color{Qt::gray};
    QPixmap m_image;
    bool m_hasImage = false;
};

} // namespace CollabText::Ui
