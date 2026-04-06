#pragma once
#include <QWidget>
#include <string>

namespace CollabText::Ui {

class PresenceIndicator : public QWidget {
    Q_OBJECT
public:
    explicit PresenceIndicator(QWidget *parent = nullptr);
    void setActivity(const std::string &activity);
    void setStale(bool stale);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QColor currentColor() const;
    std::string m_activity = "idle";
    bool m_stale = false;
};

} // namespace CollabText::Ui
